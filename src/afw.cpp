#include "build_config.h"
#include "afw.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "audio_cue.h"
#include "camera_interceptor.h"
#include "logger.h"
#include "settings.h"

namespace vrmod {

// AFW (docs/AFW.md): under AER each eye gets a new image every other frame; the eye not rendered kept its old image,
// which the compositor corrects for head rotation only -- not for the game camera moving (stick turns, walking), so it
// lags a frame: 36 Hz judder. AFW rebuilds that eye every frame from the eye just rendered: both views share the
// orientation and frustum and differ by the eye separation along the camera's x axis, so each pixel moves sideways by
// its disparity, f_px * baseline / z. Depth comes from the game's depth buffer at the DLSS evaluate of that frame
// (render resolution, linearised there with the frame's near/far); the warp runs at Present on our copy of the
// backbuffer: clear -> splat the nearest depth per target pixel -> resolve colours -> fill disocclusions from the
// farther (background) neighbour along the row.
namespace {

const char* kShader = R"(
cbuffer C : register(b0) {
    uint W; uint H; uint RW; uint RH;
    float Scale;      // f_px * baseline (px * cm): target x = x - Scale / z
    float Near; float Far; uint PassId;
    float3 R1; float pad1; float3 F1; float pad2; float3 U1; float pad3;   // the warped eye's last real camera (axes)
    float3 R0; float pad4; float3 F0; float pad5; float3 U0; float pad6;   // the warped eye's camera now (axes)
    float3 Delta; float TanH;                                             // last camera position - now (cm); frustum
    float TanV; float3 pad7;
};
Texture2D<float>  Depth  : register(t0);   // game depth (render res) -- linearise pass
Texture2D<float>  Lin    : register(t1);   // linear depth, cm (render res)
Texture2D<float4> Src    : register(t2);   // the rendered eye (output res)
Texture2D<float4> Last   : register(t3);   // the warped eye's own last real image (disocclusion fill); Near > 0.5 = present
RWTexture2D<float> LinOut : register(u0);
RWByteAddressBuffer Z    : register(u1);   // per target pixel: nearest depth (asuint), 0xFFFFFFFF = hole
RWTexture2D<float4> Out  : register(u2);

float LinAt(uint2 p) {
    uint2 q = uint2(min((p.x * RW + RW / 2) / W, RW - 1), min((p.y * RH + RH / 2) / H, RH - 1));
    return Lin.Load(int3(q, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (PassId == 0) {   // linearise (render res); the game's depth is reversed (1 near, 0 far)
        if (id.x >= RW || id.y >= RH) return;
        // Nearest depth of the 3x3 neighbourhood: foreground edges grow by one render pixel, so the anti-aliased
        // edge pixels of the (stable, full-resolution) image move with the object instead of flipping with DLSS's
        // per-frame jitter of the depth buffer (edge shimmer).
        float d = 0.0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                d = max(d, Depth.Load(int3(clamp((int)id.x + dx, 0, (int)RW - 1), clamp((int)id.y + dy, 0, (int)RH - 1), 0)));
        float z = (d > 1e-7) ? Near * Far / (d * (Far - Near) + Near) : Far;
        LinOut[id.xy] = z;
        return;
    }
    if (id.x >= W || id.y >= H) return;
    uint i = (id.y * W + id.x) * 4;
    if (PassId == 1) { Z.Store(i, 0xFFFFFFFF); return; }
    if (PassId == 2 || PassId == 3) {
        float z = LinAt(id.xy);
        float xt = (float)id.x + 0.5 - Scale / z;
        if (xt < 0.0 || xt >= (float)W) return;
        uint t = (id.y * W + (uint)xt) * 4;
        if (PassId == 2) { uint prev; Z.InterlockedMin(t, asuint(z), prev); return; }
        if (Z.Load(t) == asuint(z)) Out[uint2((uint)xt, id.y)] = Src.Load(int3(id.xy, 0));
        return;
    }
    // PassId 4: holes are disocclusions -- background the rendered eye couldn't see. The warped eye's own last real
    // image (one frame old) saw it; without one, the farther filled neighbour along the row.
    if (Z.Load(i) != 0xFFFFFFFF) return;
    int bestX = -1; float bestZ = -1.0;
    for (int s = 1; s <= 768; ++s) {
        int xl = (int)id.x - s, xr = (int)id.x + s;
        if (xl >= 0) { uint zl = Z.Load((id.y * W + xl) * 4); if (zl != 0xFFFFFFFF) { float f = asfloat(zl); if (f > bestZ) { bestZ = f; bestX = xl; } } }
        if (xr < (int)W) { uint zr = Z.Load((id.y * W + xr) * 4); if (zr != 0xFFFFFFFF) { float f = asfloat(zr); if (f > bestZ) { bestZ = f; bestX = xr; } } }
        if (bestX >= 0 && s >= 8) break;   // nearest few on both sides considered, then stop
    }
    if (Near > 0.5) {
        // The last real image, reprojected: this pixel at the depth of the background beside it, seen from the camera
        // now, looked up where that point was in the last real frame.
        float z = bestX >= 0 ? bestZ : Far;
        float nx = ((float)id.x + 0.5) / (float)W * 2.0 - 1.0, ny = 1.0 - ((float)id.y + 0.5) / (float)H * 2.0;
        float3 P = R0 * (nx * TanH * z) + F0 * z + U0 * (ny * TanV * z) - Delta;
        float zz = dot(P, F1);
        if (zz > 1.0) {
            float px = (dot(P, R1) / (zz * TanH) + 1.0) * 0.5 * (float)W;
            float py = (1.0 - dot(P, U1) / (zz * TanV)) * 0.5 * (float)H;
            if (px >= 0.0 && py >= 0.0 && px < (float)W && py < (float)H) { Out[id.xy] = Last.Load(int3((int)px, (int)py, 0)); return; }
        }
        Out[id.xy] = Last.Load(int3(id.xy, 0));
        return;
    }
    if (bestX >= 0) Out[id.xy] = Out[uint2(bestX, id.y)];
    else Out[id.xy] = Src.Load(int3(id.xy, 0));
}
)";

constexpr int kSlots = 4;
struct Slot { ID3D12Resource* lin = nullptr; uint64_t frame = 0; uint32_t rw = 0, rh = 0; float farCm = 0.0f; };

ID3D12Device* g_dev = nullptr;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_heapEval = nullptr;   // per slot: t0 depth, t1 (unused), t2 (unused), u0 lin, u1, u2 -> 6 each
ID3D12DescriptorHeap* g_heapWarp = nullptr;   // per slot: 6
UINT g_inc = 0;
bool g_initFailed = false;
Slot g_slots[kSlots];
ID3D12Resource* g_src = nullptr;   // copy of the backbuffer
ID3D12Resource* g_out = nullptr;   // warped eye
ID3D12Resource* g_z = nullptr;     // nearest-depth buffer
ID3D12Resource* g_last[2] = {};    // each eye's last real image (the fill for its warped frames)
bool g_lastValid[2] = {};
uint64_t g_lastFrame[2] = {};      // the game frame of each eye's last real image

// Camera axes from the game's local->world quaternion (local X right, Y forward, Z up).
void Axes(const float q[4], float* r, float* f, float* u) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y + w * z); r[2] = 2 * (x * z - w * y);
    f[0] = 2 * (x * y - w * z); f[1] = 1 - 2 * (x * x + z * z); f[2] = 2 * (y * z + w * x);
    u[0] = 2 * (x * z + w * y); u[1] = 2 * (y * z - w * x); u[2] = 1 - 2 * (x * x + y * y);
}
uint32_t g_W = 0, g_H = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_warps{0}, g_noDepth{0}, g_evals{0};

DXGI_FORMAT DepthSrv(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return f;
    }
}

// A typed, UAV-capable format of the backbuffer's family (raw bits kept; sRGB views are never UAVs).
DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_UNKNOWN;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

bool Init(ID3D12Device* dev) {
    if (g_pso) return true;
    if (g_initFailed) return false;
    g_initFailed = true;
    g_dev = dev;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!compile) { modlog::Line("AFW: d3dcompiler_47.dll not available."); return false; }
    ID3DBlob* cs = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(compile(kShader, strlen(kShader), "afw", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
        modlog::Line(std::string("AFW: shader compile failed: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        if (err) err->Release();
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 3;
    ranges[1].OffsetInDescriptorsFromTableStart = 4;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 40;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    using Serialize_t = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    const auto serialize = reinterpret_cast<Serialize_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
    ID3DBlob* rsBlob = nullptr;
    if (!serialize || FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err)) ||
        FAILED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_root)))) {
        modlog::Line("AFW: root signature failed.");
        if (err) err->Release();
        if (rsBlob) rsBlob->Release();
        cs->Release();
        return false;
    }
    rsBlob->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_root;
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    const HRESULT hp = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    cs->Release();
    if (FAILED(hp)) { modlog::Line("AFW: compute PSO failed."); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 7 * kSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heapEval))) || FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heapWarp)))) {
        modlog::Line("AFW: descriptor heaps failed.");
        return false;
    }
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_initFailed = false;
    modlog::Line("AFW: ready.");
    return true;
}

ID3D12Resource* Tex(uint32_t w, uint32_t h, DXGI_FORMAT f, bool uav, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = f; d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    return SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r))) ? r : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, int i) { D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart(); c.ptr += static_cast<SIZE_T>(i) * g_inc; return c; }
D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, int i) { D3D12_GPU_DESCRIPTOR_HANDLE g = h->GetGPUDescriptorHandleForHeapStart(); g.ptr += static_cast<UINT64>(i) * g_inc; return g; }

void NullSrv(D3D12_CPU_DESCRIPTOR_HANDLE c) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(nullptr, &sv, c);
}
void NullUavTex(D3D12_CPU_DESCRIPTOR_HANDLE c, DXGI_FORMAT f) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = f;
    g_dev->CreateUnorderedAccessView(nullptr, nullptr, &uv, c);
}
void NullUavBuf(D3D12_CPU_DESCRIPTOR_HANDLE c) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.Buffer.NumElements = 1;
    uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_dev->CreateUnorderedAccessView(nullptr, nullptr, &uv, c);
}

void UavBarrier(ID3D12GraphicsCommandList* cl) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = nullptr;
    cl->ResourceBarrier(1, &b);
}

void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x = {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &x);
}

}  // namespace

bool AfwEnabled() { return g_on.load(std::memory_order_relaxed); }

// The warp assumes both eyes look the same way (it moves pixels sideways only). A headset whose eye views are rotated
// against each other (canted displays reported as they are -- e.g. Pimax with parallel projection off) would show the
// warped eye turned by that angle every other frame: frame warp stands down while they are.
std::atomic<bool> g_canted{ false };

void AfwSetEyeViewAngle(float degrees) {
    const bool canted = degrees > 0.5f;
    if (g_canted.exchange(canted) == canted) return;
    char b[200];
    snprintf(b, sizeof(b), canted ? "AFW: the headset's eye views are %.1f deg apart (canted displays, e.g. Pimax parallel projection off) -- "
             "frame warp stands down (it assumes parallel eyes); plain AER" : "AFW: the headset's eye views are parallel again (%.1f deg) -- frame warp available", degrees);
    modlog::Line(b);
}

bool AfwCantedEyes() { return g_canted.load(std::memory_order_relaxed); }

void AfwSetEnabled(bool on) {
    if (g_on.exchange(on) == on) return;
    modlog::Line(std::string("AFW: ") + (on ? "ON -- the eye not rendered this frame is warped from the one that was" : "off (plain AER)") + " (overlay)");
}

void AfwOnEvaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, uint32_t rw, uint32_t rh, uint64_t frame) {
    if (!g_on.load(std::memory_order_relaxed) || g_canted.load(std::memory_order_relaxed) || !list || !depth || !frame) return;
    CamFrameRec rec{};
    if (!CameraFrameRecord(frame, &rec) || rec.nearCm <= 0.0f || rec.farCm <= rec.nearCm) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return;
    const bool ok = Init(dev);
    dev->Release();
    if (!ok) return;
    const int si = static_cast<int>(frame % kSlots);
    Slot& s = g_slots[si];
    if (!s.lin || s.rw != rw || s.rh != rh) {
        if (s.lin) s.lin->Release();
        s.lin = Tex(rw, rh, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s.rw = rw; s.rh = rh;
        if (!s.lin) return;
    } else {
        Transition(list, s.lin, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const D3D12_RESOURCE_DESC dd = depth->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Format = DepthSrv(dd.Format);
    sv.Texture2D.MipLevels = 1;
    const int b = si * 7;
    g_dev->CreateShaderResourceView(depth, &sv, Cpu(g_heapEval, b + 0));
    NullSrv(Cpu(g_heapEval, b + 1));
    NullSrv(Cpu(g_heapEval, b + 2));
    NullSrv(Cpu(g_heapEval, b + 3));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    g_dev->CreateUnorderedAccessView(s.lin, nullptr, &uv, Cpu(g_heapEval, b + 4));
    NullUavBuf(Cpu(g_heapEval, b + 5));
    NullUavTex(Cpu(g_heapEval, b + 6), DXGI_FORMAT_R8G8B8A8_UNORM);
    uint32_t c[40] = { 0, 0, rw, rh };
    memcpy(&c[5], &rec.nearCm, 4);
    memcpy(&c[6], &rec.farCm, 4);
    list->SetDescriptorHeaps(1, &g_heapEval);
    list->SetComputeRootSignature(g_root);
    list->SetComputeRoot32BitConstants(0, 40, c, 0);
    list->SetComputeRootDescriptorTable(1, Gpu(g_heapEval, b));
    list->SetPipelineState(g_pso);
    list->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
    Transition(list, s.lin, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.frame = frame;
    s.farCm = rec.farCm;
    g_evals.fetch_add(1, std::memory_order_relaxed);
}

bool AfwRecordWarp(ID3D12GraphicsCommandList* cl, ID3D12Resource* backbuffer, ID3D12Resource* dst, uint64_t frame, float baselineCm,
                   float tanH, float tanV, int renderedEye) {
    if (!g_on.load(std::memory_order_relaxed) || !g_pso || !cl || !backbuffer || !dst) return false;
    const Slot* s = nullptr;
    for (const Slot& x : g_slots) if (x.lin && x.frame == frame) s = &x;
    if (!s) { g_noDepth.fetch_add(1, std::memory_order_relaxed); return false; }
    const D3D12_RESOURCE_DESC bd = backbuffer->GetDesc();
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();
    // AER: the whole frame is one eye. The swapchain image may be larger (it has a minimum capacity, 4320 tall, and the
    // eye image goes into its top-left corner): until 2026-10-27 only an exact match was warped, so below 4320 tall --
    // 100 % on most headsets -- frame warp silently never ran and the eyes showed a shaking double image.
    const uint32_t W = static_cast<uint32_t>(bd.Width), H = bd.Height;
    if (static_cast<uint32_t>(dd.Width) < W || dd.Height < H) return false;
    const DXGI_FORMAT tf = Typed(bd.Format);
    if (tf == DXGI_FORMAT_UNKNOWN) return false;
    if (!g_src || g_W != W || g_H != H || g_fmt != bd.Format) {
        if (g_src) g_src->Release();
        if (g_out) g_out->Release();
        if (g_z) g_z->Release();
        for (int e = 0; e < 2; ++e) {
            if (g_last[e]) g_last[e]->Release();
            g_last[e] = Tex(W, H, bd.Format, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_lastValid[e] = false;
        }
        g_src = Tex(W, H, bd.Format, false, D3D12_RESOURCE_STATE_COPY_DEST);
        g_out = Tex(W, H, tf, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC zd = {};
        zd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        zd.Width = static_cast<UINT64>(W) * H * 4;
        zd.Height = 1; zd.DepthOrArraySize = 1; zd.MipLevels = 1; zd.SampleDesc.Count = 1;
        zd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        zd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        g_z = nullptr;
        g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &zd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_z));
        g_W = W; g_H = H; g_fmt = bd.Format;
        if (!g_src || !g_out || !g_z || !g_last[0] || !g_last[1]) { modlog::Line("AFW: could not create the warp textures."); g_on.store(false); return false; }
        char b[160];
        snprintf(b, sizeof(b), "AFW: warp textures %ux%u, backbuffer format %d (typed %d)", W, H, static_cast<int>(bd.Format), static_cast<int>(tf));
        modlog::Line(b);
    } else {
        Transition(cl, g_src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(cl, g_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    cl->CopyResource(g_src, backbuffer);   // backbuffer is in COPY_SOURCE here
    Transition(cl, g_src, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // The rendered eye's real image, kept as the fill for its own warped frame next time.
    const int A = renderedEye & 1, B = 1 - A;
    Transition(cl, g_last[A], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(g_last[A], backbuffer);
    Transition(cl, g_last[A], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const int si = static_cast<int>(s - g_slots);
    const int b = si * 7;
    NullSrv(Cpu(g_heapWarp, b + 0));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    g_dev->CreateShaderResourceView(s->lin, &sv, Cpu(g_heapWarp, b + 1));
    sv.Format = tf;
    g_dev->CreateShaderResourceView(g_src, &sv, Cpu(g_heapWarp, b + 2));
    g_dev->CreateShaderResourceView(g_last[B], &sv, Cpu(g_heapWarp, b + 3));
    NullUavTex(Cpu(g_heapWarp, b + 4), DXGI_FORMAT_R32_FLOAT);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.Buffer.NumElements = W * H;
    uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_dev->CreateUnorderedAccessView(g_z, nullptr, &uv, Cpu(g_heapWarp, b + 5));
    uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = tf;
    g_dev->CreateUnorderedAccessView(g_out, nullptr, &uv, Cpu(g_heapWarp, b + 6));

    uint32_t c[40] = { W, H, s->rw, s->rh };
    const float scalePxCm = (0.5f * static_cast<float>(W) / tanH) * baselineCm;
    memcpy(&c[4], &scalePxCm, 4);
    // The fill: the warped eye's last real image, reprojected from that frame's camera to this eye's camera now (this
    // frame's camera moved by the baseline along its own x axis).
    float haveLast = 0.0f;
    CamFrameRec now{}, last{};
    if (g_lastValid[B] && CameraFrameRecord(frame, &now) && CameraFrameRecord(g_lastFrame[B], &last)) {
        float f[40] = {};
        Axes(last.q, &f[8], &f[12], &f[16]);
        Axes(now.q, &f[20], &f[24], &f[28]);
        const double nowPos[3] = { now.pos[0] + static_cast<double>(f[20]) * baselineCm, now.pos[1] + static_cast<double>(f[21]) * baselineCm,
                                   now.pos[2] + static_cast<double>(f[22]) * baselineCm };
        for (int k = 0; k < 3; ++k) f[32 + k] = static_cast<float>(last.pos[k] - nowPos[k]);
        f[35] = tanH;
        f[36] = tanV;
        memcpy(&c[8], &f[8], 4 * 32);
        haveLast = 1.0f;
    }
    memcpy(&c[5], &haveLast, 4);
    const float farCm = s->farCm;
    memcpy(&c[6], &farCm, 4);
    cl->SetDescriptorHeaps(1, &g_heapWarp);
    cl->SetComputeRootSignature(g_root);
    cl->SetComputeRootDescriptorTable(1, Gpu(g_heapWarp, b));
    cl->SetPipelineState(g_pso);
    for (uint32_t pass = 1; pass <= 4; ++pass) {
        c[7] = pass;
        cl->SetComputeRoot32BitConstants(0, 40, c, 0);
        cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        UavBarrier(cl);
    }
    Transition(cl, g_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    // dst (the other eye's swapchain image, in COPY_DEST here): the warped eye into its top-left W x H.
    if (static_cast<uint32_t>(dd.Width) == W && dd.Height == H) {
        cl->CopyResource(dst, g_out);
    } else {
        D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
        to.pResource = dst;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = g_out;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box = { 0, 0, 0, W, H, 1 };
        cl->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    }
    g_lastValid[A] = true;
    g_lastFrame[A] = frame;
    g_warps.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool AfwReady(uint64_t frame) {
    if (!g_on.load(std::memory_order_relaxed) || g_canted.load(std::memory_order_relaxed) || !g_pso) return false;
    for (const Slot& x : g_slots) if (x.lin && x.frame == frame) return true;
    g_noDepth.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void AfwOnPresent() {
    static bool loaded = false;
    if (!loaded) { loaded = true; g_on.store(GetAfwSetting()); }
    static bool iWas = false;
    const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool i = (VRMOD_DEV_KEY('I') & 0x8000) != 0;
    if (i && !iWas && ca) {
        const bool on = !g_on.load();
        g_on.store(on);
        modlog::Line(std::string("AFW: ") + (on ? "ON -- the eye not rendered this frame is warped from the one that was" : "off (plain AER)") + " (Ctrl+Alt+I)");
        PlayCue(on ? Cue::On : Cue::Off);
    }
    iWas = i;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 10000) {
        last = now;
        const uint64_t w = g_warps.exchange(0), nd = g_noDepth.exchange(0), ev = g_evals.exchange(0);
        if (g_on.load() || w) {
            char b[200];
            snprintf(b, sizeof(b), "AFW (10 s, %s): %llu eyes warped, %llu without depth for their frame, %llu depth captures",
                     g_on.load() ? "on" : "off", static_cast<unsigned long long>(w), static_cast<unsigned long long>(nd), static_cast<unsigned long long>(ev));
            modlog::Line(b);
        }
    }
}

}  // namespace vrmod
