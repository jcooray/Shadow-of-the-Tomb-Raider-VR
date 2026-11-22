#include "build_config.h"
#include "hud_layer.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#include "aer.h"
#include "audio_cue.h"
#include "camera_interceptor.h"
#include "logger.h"
#include "settings.h"
#include "ui_probe.h"

// The game's final composite (RenderDoc, frame 10430, event 20575, PS "GammaBuffer" / uiTexture): the tone-mapped scene
// (LUT, linear Rec709) * (1 - ui.a) + ui.rgb, with ui sampled through an sRGB view (linear, premultiplied); a later
// pass gamma-encodes into the swapchain. So the HUD is blended in linear light: with an sRGB render-target view on the
// eye image the hardware does exactly that (blend ONE / INV_SRC_ALPHA); with only a UNORM view the shader encodes the
// HUD colour with gamma 2.2 and the blend happens in gamma space (close; differs only on semi-transparent edges).

namespace vrmod {
namespace {

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

const char kShader[] = R"(
Texture2D<float4> hud : register(t0);
SamplerState lin : register(s0);
cbuffer C : register(b0) { uint mode; float shiftU; uint tint; uint copy; float4 src; float4 hole; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target {
    // copy (the HUD panel / the reticle): the target shows the source rectangle src (x, y, w, h in the layer's uv), and
    // nothing inside hole (x0, y0, x1, y1) -- the reticle's box, shown by its own quad.
    float2 uv = copy != 0 ? src.xy + i.uv * src.zw : i.uv + float2(shiftU, 0);
    float4 c = hud.SampleLevel(lin, uv, 0);
    if (copy != 0 && uv.x > hole.x && uv.y > hole.y && uv.x < hole.z && uv.y < hole.w) c = 0;
    if (tint != 0) c.rgb *= float3(1.0, 0.15, 0.15);   // test: our layer shown red
    if (mode == 1) {
        float a = c.a;
        float3 u = a > 1e-4 ? c.rgb / a : 0;
        c.rgb = pow(saturate(u), 1.0 / 2.2) * a;
    }
    return c;
}
)";

std::atomic<bool> g_on{ false };
std::atomic<bool> g_tint{ false };   // test: our layer drawn red (Ctrl+Alt+L cycle)
std::atomic<uint64_t> g_statEye[3] = {};   // composites by the frame's rendered eye: left, right, both (mono)
std::atomic<bool> g_loaded{ false };
std::mutex g_mutex;   // creation
ID3D12Device* g_dev = nullptr;
std::atomic<const void*> g_gameLayer{ nullptr };
ID3D12Resource* g_tex = nullptr;          // ours: R8G8B8A8_TYPELESS, game layer size, RENDER_TARGET between composites
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;   // 0: unused, 1: left store, 2: right store
// Per-eye HUD (2026-10-08 test): under AER the game camera alternates between the eyes, and the game places world icons
// with it -- so the same HUD in both eyes made icons jump sideways every frame (the flicker the user saw, with or
// without the layer). Each eye now shows the HUD of the last frame rendered for that eye: icons sit on their objects with
// the right disparity. Stores live in PIXEL_SHADER_RESOURCE between composites.
ID3D12Resource* g_store[2] = {};
bool g_storeFilled[2] = {};
std::atomic<uint64_t> g_ourRtv{ 0 };
ID3D12RootSignature* g_root = nullptr;
ID3D12DescriptorHeap* g_eyeRtvHeap = nullptr;   // RTVs on the eye images (cached by resource + format)
constexpr int kEyeRtvMax = 48;   // eye images, and the panel's and the reticle's
struct EyeRtv { ID3D12Resource* res; DXGI_FORMAT fmt; };
EyeRtv g_eyeRtv[kEyeRtvMax] = {};
int g_eyeRtvNext = 0;
UINT g_rtvInc = 0;
ID3D12PipelineState* g_psoSrgb = nullptr;   // RTV R8G8B8A8_UNORM_SRGB, mode 0
ID3D12PipelineState* g_psoUnorm = nullptr;  // RTV R8G8B8A8_UNORM, mode 1
ID3D12PipelineState* g_psoCopySrgb = nullptr;    // the same two without blending: the panel / reticle images are written whole
ID3D12PipelineState* g_psoCopyUnorm = nullptr;

// HUD panel (hud_layer.h). World icons get a texture of their own (they stay in the eye images, per eye); the stores
// then hold them instead of the whole HUD.
ID3D12Resource* g_world = nullptr;
std::atomic<uint64_t> g_worldRtv{ 0 };
std::atomic<bool> g_panelFailed{ false };
std::atomic<bool> g_hudDrawn{ false };     // a HUD draw into the layer since the panel was last updated
std::atomic<bool> g_worldDrawn{ false };   // a world-icon draw into its texture since the last composite
std::atomic<uint64_t> g_statPanel{ 0 }, g_statReticle{ 0 }, g_statPanelEmpty{ 0 };
std::mutex g_reticleMutex;
struct ReticleRec { uint64_t frame; HudReticle r; };
ReticleRec g_reticleRing[16] = {};
int g_reticleNext = 0;
std::atomic<bool> g_reticleNow{ false };

bool PanelOn() {
    return g_on.load(std::memory_order_relaxed) && !g_panelFailed.load(std::memory_order_relaxed) && SettingGet("hud.panel") != 0.0f;
}
bool g_pipelineFailed = false;

std::atomic<double> g_compositeAliveMs{ 0.0 };   // the Present path asked to composite recently (else don't take the HUD)
std::atomic<bool> g_redirected{ false };          // a redirect happened since the last composite
std::atomic<uint64_t> g_statRedirects{ 0 }, g_statComposites{ 0 }, g_statSkipped{ 0 }, g_statWorld{ 0 }, g_statClears{ 0 };
std::atomic<uint64_t> g_statNoClear{ 0 };   // composites with no clear of the layer since the last one (the HUD piles up)
std::atomic<bool> g_cleared{ false };       // a redirected clear since the last composite

void LoadOnce() {
    if (g_loaded.exchange(true)) return;
    g_on.store(GetHudLayerSetting());
}

bool CreateTexture(ID3D12Resource* layer) {
    const D3D12_RESOURCE_DESC ld = layer->GetDesc();
    ID3D12Device* dev = nullptr;
    if (FAILED(layer->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false;
    g_dev = dev;   // kept (one device for the game's lifetime)
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = ld.Width;
    d.Height = ld.Height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&g_tex)))) return false;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&g_world)))) g_world = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 2;
    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 3;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap))) || FAILED(dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srvHeap)))) return false;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(g_tex, &rv, rtv);
    if (g_world) {
        D3D12_CPU_DESCRIPTOR_HANDLE w = rtv;
        w.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dev->CreateRenderTargetView(g_world, &rv, w);
        g_worldRtv.store(w.ptr);
    } else {
        modlog::Line("HUDLAYER: no texture for the world icons -- the HUD stays in the eye images (no panel).");
        g_panelFailed.store(true);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(g_tex, &sv, g_srvHeap->GetCPUDescriptorHandleForHeapStart());
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int e = 0; e < 2; ++e) {
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_store[e])))) return false;
        D3D12_CPU_DESCRIPTOR_HANDLE c = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        c.ptr += static_cast<SIZE_T>(1 + e) * inc;
        dev->CreateShaderResourceView(g_store[e], &sv, c);
    }
    g_ourRtv.store(rtv.ptr);
    modlog::Line("HUDLAYER: the game's HUD layer is " + std::to_string(ld.Width) + "x" + std::to_string(ld.Height)
        + "; its draws go into our layer and it is added over each eye after the warp ([hud] layer, Ctrl+Alt+L)");
    return true;
}

ID3D12PipelineState* MakePso(ID3D12Device* dev, ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv, bool blend = true) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = g_root;
    p.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    p.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    D3D12_RENDER_TARGET_BLEND_DESC& b = p.BlendState.RenderTarget[0];
    b.BlendEnable = blend ? TRUE : FALSE;
    b.SrcBlend = D3D12_BLEND_ONE;
    b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO;   // the eye image keeps its alpha
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = 0xFFFFFFFFu;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = rtv;
    p.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(dev->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)))) return nullptr;
    UiProbeMarkNotUi(pso);   // same signature as the game's UI pipelines: keep the HUD scaling and layer learning off it
    return pso;
}

bool Pipeline() {
    if (g_psoSrgb && g_psoUnorm && g_psoCopySrgb && g_psoCopyUnorm) return true;
    if (g_pipelineFailed || !g_dev) return false;
    g_pipelineFailed = true;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!compile) { modlog::Line("HUDLAYER: d3dcompiler_47.dll not available -- off."); return false; }
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(compile(kShader, strlen(kShader), "hud", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err))
        || FAILED(compile(kShader, strlen(kShader), "hud", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err))) {
        modlog::Line(std::string("HUDLAYER: shader compile failed: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        return false;
    }
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 12;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samp.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    using Serialize_t = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    const auto serialize = reinterpret_cast<Serialize_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
    ID3DBlob* rsBlob = nullptr;
    if (!serialize || FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err))
        || FAILED(g_dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_root)))) {
        modlog::Line("HUDLAYER: root signature failed -- off.");
        return false;
    }
    rsBlob->Release();
    g_psoSrgb = MakePso(g_dev, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    g_psoUnorm = MakePso(g_dev, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM);
    g_psoCopySrgb = MakePso(g_dev, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false);
    g_psoCopyUnorm = MakePso(g_dev, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    vs->Release();
    ps->Release();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kEyeRtvMax;
    if (!g_psoSrgb || !g_psoUnorm || !g_psoCopySrgb || !g_psoCopyUnorm || FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_eyeRtvHeap)))) {
        modlog::Line("HUDLAYER: pipeline failed -- off.");
        return false;
    }
    g_rtvInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_pipelineFailed = false;
    return true;
}

// The eye image's RTV and which pipeline: an sRGB view when the image's format allows one (typeless or sRGB), else UNORM.
bool EyeTarget(ID3D12Resource* eye, D3D12_CPU_DESCRIPTOR_HANDLE* rtv, ID3D12PipelineState** pso, uint32_t* mode, bool copy = false) {
    const DXGI_FORMAT f = eye->GetDesc().Format;
    DXGI_FORMAT view;
    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { view = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; *pso = copy ? g_psoCopySrgb : g_psoSrgb; *mode = 0; }
    else if (f == DXGI_FORMAT_R8G8B8A8_UNORM) { view = DXGI_FORMAT_R8G8B8A8_UNORM; *pso = copy ? g_psoCopyUnorm : g_psoUnorm; *mode = 1; }
    else return false;
    int slot = -1;
    for (int i = 0; i < kEyeRtvMax; ++i) if (g_eyeRtv[i].res == eye && g_eyeRtv[i].fmt == view) slot = i;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_eyeRtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (slot < 0) {
        slot = g_eyeRtvNext;
        g_eyeRtvNext = (g_eyeRtvNext + 1) % kEyeRtvMax;
        g_eyeRtv[slot] = { eye, view };
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = view;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        h.ptr += static_cast<SIZE_T>(slot) * g_rtvInc;
        g_dev->CreateRenderTargetView(eye, &rv, h);
    } else {
        h.ptr += static_cast<SIZE_T>(slot) * g_rtvInc;
    }
    *rtv = h;
    return true;
}

}  // namespace

void HudLayerSeeUiDraw(ID3D12Resource* layer) {
    LoadOnce();
    if (layer && g_gameLayer.load(std::memory_order_relaxed) == layer) g_hudDrawn.store(true, std::memory_order_relaxed);
    if (!layer || !g_on.load(std::memory_order_relaxed) || g_gameLayer.load(std::memory_order_relaxed) == layer) return;
    if (MenuScreenActive()) return;   // learn from gameplay HUD only
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_gameLayer.load() == layer) return;
    if (!g_tex) {
        if (!CreateTexture(layer)) { modlog::Line("HUDLAYER: could not create our layer -- off."); g_on.store(false); return; }
    } else {
        const D3D12_RESOURCE_DESC a = layer->GetDesc(), b = g_tex->GetDesc();
        if (a.Width != b.Width || a.Height != b.Height) {
            modlog::Line("HUDLAYER: the game's HUD layer changed size -- not redirected this session.");
            return;
        }
    }
    g_gameLayer.store(layer);
}

bool HudLayerRedirect(const void* res, D3D12_CPU_DESCRIPTOR_HANDLE* ours, bool clear) {
    if (!res || res != g_gameLayer.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed)) return false;
    if (MenuScreenActive() || NowMs() - g_compositeAliveMs.load(std::memory_order_relaxed) > 500.0) return false;
    const uint64_t rtv = g_ourRtv.load(std::memory_order_relaxed);
    if (!rtv) return false;
    ours->ptr = static_cast<SIZE_T>(rtv);
    g_redirected.store(true, std::memory_order_relaxed);
    if (clear) {
        g_cleared.store(true, std::memory_order_relaxed);
        g_statClears.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_statRedirects.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool HudLayerWorldRtv(D3D12_CPU_DESCRIPTOR_HANDLE* ours) {
    if (!g_on.load(std::memory_order_relaxed) || !g_gameLayer.load(std::memory_order_relaxed)) return false;
    if (MenuScreenActive() || NowMs() - g_compositeAliveMs.load(std::memory_order_relaxed) > 500.0) return false;
    const uint64_t world = PanelOn() ? g_worldRtv.load(std::memory_order_relaxed) : 0;
    const uint64_t rtv = world ? world : g_ourRtv.load(std::memory_order_relaxed);
    if (!rtv) return false;
    ours->ptr = static_cast<SIZE_T>(rtv);
    g_redirected.store(true, std::memory_order_relaxed);
    if (world) g_worldDrawn.store(true, std::memory_order_relaxed);
    g_statWorld.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool HudLayerWorldClear(const void* res, D3D12_CPU_DESCRIPTOR_HANDLE* worldRtv) {
    if (!res || res != g_gameLayer.load(std::memory_order_relaxed)) return false;
    const uint64_t rtv = g_worldRtv.load(std::memory_order_relaxed);
    if (!rtv) return false;
    worldRtv->ptr = static_cast<SIZE_T>(rtv);
    return true;
}

bool HudLayerPanelWanted() {
    LoadOnce();
    return PanelOn();
}

void HudLayerPanelFailed(const char* why) {
    if (g_panelFailed.exchange(true)) return;
    modlog::Line(std::string("HUDLAYER: no HUD panel -- ") + why + "; the HUD goes over the eye images as before.");
}

void HudLayerPanelShape(float* scale, float* offsetDown, float* reticleHalf) {
    bool on = false;
    float s = 1.0f, o = 0.0f;
    if (!UiProbeHudGet(&on, &s, &o) || !on) { s = 1.0f; o = 0.0f; }
    *scale = s;
    *offsetDown = o;
    *reticleHalf = SettingGet("hud.reticle_box") * s;
}

bool HudLayerPanelPixels(uint32_t* panelW, uint32_t* panelH, uint32_t* reticle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_tex) return false;
    const D3D12_RESOURCE_DESC d = g_tex->GetDesc();
    float s, o, rh;
    HudLayerPanelShape(&s, &o, &rh);
    // The HUD's own pixels (the layer's, scaled), at most 2560 wide: about what a headset shows over the panel's angle.
    uint32_t w = static_cast<uint32_t>(static_cast<float>(d.Width) * s);
    if (w > 2560) w = 2560;
    if (w < 640) w = 640;
    w &= ~1u;
    uint32_t h = static_cast<uint32_t>(static_cast<float>(w) * static_cast<float>(d.Height) / static_cast<float>(d.Width)) & ~1u;
    uint32_t r = static_cast<uint32_t>(2.0f * rh * static_cast<float>(d.Height)) & ~7u;
    if (r > 768) r = 768;
    if (r < 128) r = 128;
    *panelW = w;
    *panelH = h;
    *reticle = r;
    return true;
}

void HudLayerSetReticle(uint64_t gameFrame, bool on, const float local[3], bool hasDistance, bool fromCentre) {
    g_reticleNow.store(on, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_reticleMutex);
    const int last = (g_reticleNext + 15) % 16;
    const bool again = g_reticleRing[last].frame == gameFrame;   // the frame's second camera write: the same record
    ReticleRec& rec = g_reticleRing[again ? last : g_reticleNext];
    if (!again) g_reticleNext = (g_reticleNext + 1) % 16;
    rec.frame = gameFrame;
    rec.r.on = on;
    rec.r.local[0] = local ? local[0] : 0.0f;
    rec.r.local[1] = local ? local[1] : 1.0f;
    rec.r.local[2] = local ? local[2] : 0.0f;
    rec.r.hasDistance = hasDistance;
    rec.r.fromCentre = fromCentre;
}

bool HudLayerReticleFor(uint64_t gameFrame, HudReticle* out) {
    std::lock_guard<std::mutex> lock(g_reticleMutex);
    for (const ReticleRec& rec : g_reticleRing) {
        if (rec.frame == gameFrame && gameFrame != 0) { *out = rec.r; return true; }
    }
    return false;
}

HudPanelPlan HudLayerPanelPlan(int renderedEye, bool allow, bool allowReticle) {
    LoadOnce();
    HudPanelPlan p;
    if (!allow || !PanelOn() || !g_tex || !g_world) { p.empty = true; return p; }   // not this frame: the eye images, as before
    p.active = true;
    if (!g_redirected.load(std::memory_order_relaxed)) { p.empty = true; return p; }   // a menu, a loading screen: no HUD of ours
    // One HUD for both eyes: the left eye's frames' (the game places prompts on things with the frame's camera, and
    // the two eyes' alternating would show them twice). The right eye's frames leave the panel as it is.
    if (renderedEye > 0) return p;
    if (!g_hudDrawn.exchange(false, std::memory_order_relaxed)) {
        p.empty = true;
        g_statPanelEmpty.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    p.panel = true;
    p.reticle = allowReticle && g_reticleNow.load(std::memory_order_relaxed);
    return p;
}

void HudLayerComposite(ID3D12GraphicsCommandList* cl, ID3D12Resource* const eyes[2], int renderedEye, UINT eyeW, UINT eyeH,
                       HudPanelTargets* panel) {
    LoadOnce();
    if (!g_on.load(std::memory_order_relaxed)) return;
    g_compositeAliveMs.store(NowMs(), std::memory_order_relaxed);
    if (!g_redirected.exchange(false, std::memory_order_relaxed) || !g_tex || !g_store[0] || !Pipeline()) return;
    if (!g_cleared.exchange(false, std::memory_order_relaxed)) g_statNoClear.fetch_add(1, std::memory_order_relaxed);
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES z, D3D12_RESOURCE_BARRIER& x) {
        x = {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = r;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = a;
        x.Transition.StateAfter = z;
    };
    // Into the eye images: the whole HUD (as until 1.1.1), or -- on the panel route -- the world icons only. This frame's
    // -> the store of the eye it was rendered for (both when mono). On the panel route a frame without a world icon
    // leaves that eye's store out (nothing to add over the eye).
    ID3D12Resource* const perEye = panel ? g_world : g_tex;
    const bool fill = !panel || g_worldDrawn.exchange(false, std::memory_order_relaxed);
    if (fill) {
        D3D12_RESOURCE_BARRIER bs[3];
        int nb = 0;
        barrier(perEye, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE, bs[nb++]);
        for (int e = 0; e < 2; ++e) if (renderedEye < 0 || renderedEye == e) barrier(g_store[e], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST, bs[nb++]);
        cl->ResourceBarrier(nb, bs);
        for (int e = 0; e < 2; ++e) {
            if (renderedEye >= 0 && renderedEye != e) continue;
            cl->CopyResource(g_store[e], perEye);
            g_storeFilled[e] = true;
        }
        for (int i = 0; i < nb; ++i) std::swap(bs[i].Transition.StateBefore, bs[i].Transition.StateAfter);
        bs[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;   // ours goes straight back to the game's draws
        cl->ResourceBarrier(nb, bs);
    } else {
        for (int e = 0; e < 2; ++e) if (renderedEye < 0 || renderedEye == e) g_storeFilled[e] = false;
    }
    cl->SetGraphicsRootSignature(g_root);
    cl->SetDescriptorHeaps(1, &g_srvHeap);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_statEye[renderedEye < 0 ? 2 : renderedEye].fetch_add(1, std::memory_order_relaxed);
    const uint32_t tint = g_tint.load(std::memory_order_relaxed) ? 1u : 0u;
    for (int e = 0; e < 2; ++e) {
        if (!eyes[e] || !g_storeFilled[e]) continue;
        D3D12_GPU_DESCRIPTOR_HANDLE srv = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<UINT64>(1 + e) * inc;
        cl->SetGraphicsRootDescriptorTable(1, srv);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv;
        ID3D12PipelineState* pso = nullptr;
        uint32_t mode = 0;
        if (!EyeTarget(eyes[e], &rtv, &pso, &mode)) { g_statSkipped.fetch_add(1, std::memory_order_relaxed); continue; }
        // Only the eye image's sub-rectangle: the swapchain image can be larger (its capacity is at least 4320 tall), and
        // until 1.1.2 the HUD was stretched over all of it -- on headsets below that size most of the HUD, the crosshair
        // included, landed outside the part the headset shows (kvin, Pico 4: no crosshair; Genf; a Quest 3 on Reddit).
        const D3D12_RESOURCE_DESC d = eyes[e]->GetDesc();
        const UINT w = eyeW && eyeW < d.Width ? eyeW : static_cast<UINT>(d.Width);
        const UINT h = eyeH && eyeH < d.Height ? eyeH : d.Height;
        static bool logged = false;
        if (!logged) {
            logged = true;
            modlog::Line("HUDLAYER: added over " + std::to_string(w) + "x" + std::to_string(h) + " of each "
                + std::to_string(d.Width) + "x" + std::to_string(d.Height) + " swapchain image (our layer "
                + std::to_string(g_tex->GetDesc().Width) + "x" + std::to_string(g_tex->GetDesc().Height) + ")"
                + (panel ? " -- the world icons; the HUD itself is the panel's" : ""));
        }
        const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
        const D3D12_RECT sc{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
        const uint32_t consts[12] = { mode, 0, tint, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   // [1]: horizontal shift (HUD depth, later)
        cl->SetPipelineState(pso);
        cl->SetGraphicsRoot32BitConstants(0, 12, consts, 0);
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cl->DrawInstanced(3, 1, 0, 0);
        g_statComposites.fetch_add(1, std::memory_order_relaxed);
    }
    if (!panel || (!panel->panel && !panel->reticle)) return;

    // The panel route: the HUD itself -> the panel's image (its place in the layer: the scaled, centred rectangle the
    // viewport rewrite draws it into), and the box at its centre -> the reticle's image, left out of the panel then.
    float s, offDown, rh;
    HudLayerPanelShape(&s, &offDown, &rh);
    const D3D12_RESOURCE_DESC ld = g_tex->GetDesc();
    const float aspect = static_cast<float>(ld.Height) / static_cast<float>(ld.Width);
    const float cx = 0.5f, cy = 0.5f + offDown;
    const float box[4] = { cx - rh * aspect, cy - rh, cx + rh * aspect, cy + rh };   // square in pixels
    D3D12_RESOURCE_BARRIER tb;
    barrier(g_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, tb);
    cl->ResourceBarrier(1, &tb);
    cl->SetGraphicsRootDescriptorTable(1, g_srvHeap->GetGPUDescriptorHandleForHeapStart());   // slot 0: the HUD layer itself
    auto copy = [&](ID3D12Resource* target, const float src[4], const float hole[4]) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv;
        ID3D12PipelineState* pso = nullptr;
        uint32_t mode = 0;
        if (!target || !EyeTarget(target, &rtv, &pso, &mode, true)) return false;
        const D3D12_RESOURCE_DESC d = target->GetDesc();
        const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(d.Width), static_cast<float>(d.Height), 0.0f, 1.0f };
        const D3D12_RECT sc{ 0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height) };
        uint32_t consts[12] = { mode, 0, tint, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
        memcpy(&consts[4], src, 16);
        memcpy(&consts[8], hole, 16);
        cl->SetPipelineState(pso);
        cl->SetGraphicsRoot32BitConstants(0, 12, consts, 0);
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cl->DrawInstanced(3, 1, 0, 0);
        return true;
    };
    const float none[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
    if (panel->reticle) {
        const float src[4] = { box[0], box[1], box[2] - box[0], box[3] - box[1] };
        panel->reticleDrawn = copy(panel->reticle, src, none);
        if (panel->reticleDrawn) g_statReticle.fetch_add(1, std::memory_order_relaxed);
    }
    if (panel->panel) {
        const float src[4] = { cx - 0.5f * s, cy - 0.5f * s, s, s };
        panel->panelDrawn = copy(panel->panel, src, panel->reticleDrawn ? box : none);
        if (panel->panelDrawn) g_statPanel.fetch_add(1, std::memory_order_relaxed);
    }
    std::swap(tb.Transition.StateBefore, tb.Transition.StateAfter);
    cl->ResourceBarrier(1, &tb);
}

bool HudLayerEnabled() {
    LoadOnce();
    return g_on.load(std::memory_order_relaxed);
}

void HudLayerSetEnabled(bool on) {
    LoadOnce();
    g_tint.store(false);
    if (g_on.exchange(on) == on) return;
    modlog::Line(std::string("HUDLAYER: ") + (on ? "ON -- the HUD is added over each eye after the warp" : "off -- the HUD stays in the game's image (warped with it)") + " (overlay)");
}

void HudLayerOnPresent() {
    LoadOnce();
    // Ctrl+Alt+L cycles: on -> on with our layer tinted red (test: is a flickering icon in the HUD?) -> off -> on.
    static bool lWas = false;
    const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool l = ca && (VRMOD_DEV_KEY('L') & 0x8000);
    if (l && !lWas) {
        const int state = !g_on.load() ? 2 : g_tint.load() ? 1 : 0;   // 0 on, 1 on+tint, 2 off
        const int next = (state + 1) % 3;
        g_on.store(next != 2);
        g_tint.store(next == 1);
        modlog::Line(std::string("HUDLAYER: ") + (next == 0 ? "ON -- the HUD is added over each eye after the warp"
            : next == 1 ? "ON, our layer tinted RED (test: icons that turn red are in the game's HUD)" : "off -- the HUD stays in the game's image (warped with it)")
            + " (Ctrl+Alt+L)");
        PlayCue(next == 2 ? Cue::Off : Cue::On);
    }
    lWas = l;
    static double last = 0.0;
    if (NowMs() - last > 30000.0) {
        last = NowMs();
        const uint64_t r = g_statRedirects.exchange(0), c = g_statComposites.exchange(0), s = g_statSkipped.exchange(0);
        if (r || c || s || g_on.load()) {
            modlog::Line("HUDLAYER (30 s, " + std::string(g_on.load() ? "on" : "off") + "): " + std::to_string(r) + " HUD binds redirected, "
                + std::to_string(g_statClears.exchange(0)) + " clears, " + std::to_string(g_statNoClear.exchange(0)) + " frames without a clear, "
                + std::to_string(g_statWorld.exchange(0)) + " world-icon draws moved into the layer, "
                + std::to_string(c) + " eye composites (frames rendered for left " + std::to_string(g_statEye[0].exchange(0)) + ", right "
                + std::to_string(g_statEye[1].exchange(0)) + ", both " + std::to_string(g_statEye[2].exchange(0)) + ")" + (s ? ", " + std::to_string(s) + " eyes skipped (eye image format not RGBA8)" : "")
                + (PanelOn() ? "; panel: " + std::to_string(g_statPanel.exchange(0)) + " updates, " + std::to_string(g_statReticle.exchange(0))
                       + " with the reticle apart, " + std::to_string(g_statPanelEmpty.exchange(0)) + " frames with no HUD drawn"
                   : g_panelFailed.load() ? "; panel failed" : "; panel off")
                + (g_gameLayer.load() ? "" : "; the game's HUD layer not seen yet"));
        }
    }
}

}  // namespace vrmod
