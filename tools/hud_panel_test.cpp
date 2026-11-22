// Offline check of the HUD panel route in src/hud_layer.cpp (docs/HUD.md, "HUD panel"): a real D3D12 device, a stand-in
// for the game's HUD layer, and stand-ins for the eye, panel and reticle swapchain images. A "HUD" is painted into our
// layer through the same redirects the hooks use, the composite is run, and the images are read back:
//   - the panel holds the HUD's own rectangle of the layer (scale 0.6, centred), with the reticle's box left out;
//   - the reticle image holds that box;
//   - the eye images get the world icons only;
//   - without a reticle the box stays in the panel; a right-eye frame leaves the panel alone;
//   - with the panel not allowed the whole HUD goes over the eye images, as until 1.1.1.
// Usage: vrmod_hud_panel_test   (the game must not be running: it makes a device of its own)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hud_layer.h"
#include "logger.h"

namespace modlog {
void Line(const std::string& m) { printf("log: %s\n", m.c_str()); }
}  // namespace modlog

namespace {
float g_panelSetting = 1.0f;
}

namespace vrmod {
bool MenuScreenActive() { return false; }
float SettingGet(const char* key) {
    if (!strcmp(key, "hud.panel")) return g_panelSetting;
    if (!strcmp(key, "hud.reticle_box")) return 0.09f;
    if (!strcmp(key, "hud.distance_m")) return 2.0f;
    return 0.0f;
}
bool GetHudLayerSetting() { return true; }
bool UiProbeHudGet(bool* on, float* scale, float* offsetY) { *on = true; *scale = 0.6f; *offsetY = 0.0f; return true; }
void UiProbeMarkNotUi(const void*) {}
}  // namespace vrmod

namespace {
constexpr UINT kW = 1600, kH = 1200;
int g_failed = 0;
void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_failed;
}

ID3D12Device* g_dev = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
ID3D12CommandAllocator* g_alloc = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceValue = 0;
HANDLE g_event = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
UINT g_rtvInc = 0, g_rtvNext = 0;

void Run() {
    g_list->Close();
    ID3D12CommandList* l[] = { g_list };
    g_queue->ExecuteCommandLists(1, l);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_fence->SetEventOnCompletion(g_fenceValue, g_event);
    WaitForSingleObject(g_event, 10000);
    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);
}

ID3D12Resource* Texture(UINT w, UINT h, DXGI_FORMAT f) {
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r));
    return r;
}

D3D12_CPU_DESCRIPTOR_HANDLE Rtv(ID3D12Resource* r, DXGI_FORMAT view) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(g_rtvNext++) * g_rtvInc;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = view;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_dev->CreateRenderTargetView(r, &rv, h);
    return h;
}

// The texture's pixels (RGBA8 as stored), read back; the texture is in RENDER_TARGET before and after.
std::vector<uint8_t> Read(ID3D12Resource* tex) {
    const D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g_dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    Run();
    std::vector<uint8_t> out(static_cast<size_t>(d.Width) * d.Height * 4);
    uint8_t* p = nullptr;
    buf->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (UINT y = 0; y < d.Height; ++y) memcpy(&out[static_cast<size_t>(y) * d.Width * 4], p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, static_cast<size_t>(d.Width) * 4);
    buf->Unmap(0, nullptr);
    buf->Release();
    return out;
}

bool Is(const std::vector<uint8_t>& px, UINT w, UINT x, UINT y, int r, int g, int b, int a, const char* what) {
    const uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
    const bool ok = std::abs(p[0] - r) <= 3 && std::abs(p[1] - g) <= 3 && std::abs(p[2] - b) <= 3 && (a < 0 || std::abs(p[3] - a) <= 3);
    if (!ok) printf("      at (%u, %u): %d %d %d %d, wanted %d %d %d %d\n", x, y, p[0], p[1], p[2], p[3], r, g, b, a);
    Check(ok, what);
    return ok;
}

// A frame's "HUD": the game's clear and draws, through the redirects the hooks make.
void PaintHud(ID3D12Resource* layer) {
    using namespace vrmod;
    const FLOAT zero[4] = { 0, 0, 0, 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE ours{}, world{};
    if (HudLayerRedirect(layer, &ours, true)) {
        g_list->ClearRenderTargetView(ours, zero, 0, nullptr);
        if (HudLayerWorldClear(layer, &world)) g_list->ClearRenderTargetView(world, zero, 0, nullptr);
    }
    if (HudLayerRedirect(layer, &ours)) {
        // The HUD's rectangle (scale 0.6 about the centre): 320..1280 x 240..960, green; a blue mark in its corner; the
        // reticle's box (0.09 * 0.6 of the height each way from the centre: 735..865 x 535..665) red.
        const FLOAT green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 }, red[4] = { 1, 0, 0, 1 };
        const D3D12_RECT content{ 320, 240, 1280, 960 }, mark{ 320, 240, 340, 260 }, box{ 736, 536, 864, 664 };
        g_list->ClearRenderTargetView(ours, green, 1, &content);
        g_list->ClearRenderTargetView(ours, blue, 1, &mark);
        g_list->ClearRenderTargetView(ours, red, 1, &box);
        HudLayerSeeUiDraw(layer);
    }
    if (HudLayerWorldRtv(&world)) {   // a world icon: yellow, top-left of the eye
        const FLOAT yellow[4] = { 1, 1, 0, 1 };
        const D3D12_RECT icon{ 100, 100, 200, 200 };
        g_list->ClearRenderTargetView(world, yellow, 1, &icon);
    }
}

void Grey(ID3D12Resource* t, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    (void)t;
    const FLOAT grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };   // linear 0.5 -> 188 stored
    g_list->ClearRenderTargetView(rtv, grey, 0, nullptr);
}
}  // namespace

int main() {
    using namespace vrmod;
    ID3D12Debug* dbg = nullptr;
    const bool debugLayer = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg;
    if (debugLayer) dbg->EnableDebugLayer();
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)))) {
        IDXGIFactory4* f = nullptr;
        IDXGIAdapter* warp = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || FAILED(f->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
            || FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)))) {
            printf("no D3D12 device\n");
            return 2;
        }
    }
    printf("device ok, debug layer %s\n", debugLayer ? "ON" : "not installed");
    D3D12_COMMAND_QUEUE_DESC qd{};
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 16;
    g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    ID3D12Resource* layer = Texture(kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);   // the game's HUD layer (never drawn into)
    // Swapchain images as runtimes hand them out: typeless (an sRGB view) for the eyes and the panel, plain UNORM for
    // the reticle (the gamma-encoding path).
    ID3D12Resource* eyes[2] = { Texture(kW, kH, DXGI_FORMAT_R8G8B8A8_TYPELESS), Texture(kW, kH, DXGI_FORMAT_R8G8B8A8_TYPELESS) };
    D3D12_CPU_DESCRIPTOR_HANDLE eyeRtv[2] = { Rtv(eyes[0], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), Rtv(eyes[1], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) };
    ID3D12Resource* panel = Texture(960, 720, DXGI_FORMAT_R8G8B8A8_TYPELESS);
    ID3D12Resource* reticle = Texture(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM);

    HudLayerSeeUiDraw(layer);   // the first HUD draw: our textures are made
    uint32_t pw = 0, ph = 0, rs = 0;
    Check(HudLayerPanelPixels(&pw, &ph, &rs) && pw == 960 && ph == 720 && rs == 128, "panel 960 x 720 and reticle 128 for a 1600 x 1200 layer at scale 0.6");
    Check(HudLayerPanelWanted(), "the panel is wanted");

    // ---- 1: a mono frame, aiming: panel with the box left out, reticle image, world icon in the eyes
    {
        HudLayerComposite(g_list, eyes, -1, kW, kH, nullptr);   // (the Present path alive: redirects start)
        PaintHud(layer);
        Grey(eyes[0], eyeRtv[0]);
        Grey(eyes[1], eyeRtv[1]);
        const float local[3] = { 0.0f, 500.0f, 0.0f };
        HudLayerSetReticle(1, true, local, true, false);
        const HudPanelPlan plan = HudLayerPanelPlan(-1, true, true);
        Check(plan.active && plan.panel && plan.reticle && !plan.empty, "plan: panel and reticle to be drawn");
        HudPanelTargets t;
        t.panel = panel;
        t.reticle = reticle;
        HudLayerComposite(g_list, eyes, -1, kW, kH, &t);
        Run();
        Check(t.panelDrawn && t.reticleDrawn, "both drawn");
        const auto pp = Read(panel);
        Is(pp, 960, 5, 5, 0, 0, 255, 255, "panel: the HUD's corner mark at the panel's corner");
        Is(pp, 960, 200, 200, 0, 255, 0, 255, "panel: the HUD's own rectangle fills it");
        Is(pp, 960, 955, 715, 0, 255, 0, 255, "panel: ...to the far corner");
        Is(pp, 960, 480, 360, 0, 0, 0, 0, "panel: the reticle's box left out (transparent)");
        Is(pp, 960, 410, 360, 0, 255, 0, 255, "panel: just left of the box, the HUD");
        Is(pp, 960, 550, 360, 0, 255, 0, 255, "panel: just right of the box, the HUD");
        const auto rp = Read(reticle);
        Is(rp, 128, 64, 64, 255, 0, 0, 255, "reticle: the box's own picture");
        Is(rp, 128, 3, 3, 255, 0, 0, 255, "reticle: ...to its corner");
        const auto ep = Read(eyes[0]);
        Is(ep, kW, 150, 150, 255, 255, 0, -1, "eye: the world icon");
        Is(ep, kW, 800, 600, 188, 188, 188, -1, "eye: no HUD in it (the scene's grey at its centre)");
        Is(ep, kW, 400, 400, 188, 188, 188, -1, "eye: ...nor elsewhere");
        HudReticle r{};
        Check(HudLayerReticleFor(1, &r) && r.on && r.hasDistance && !r.fromCentre && r.local[1] == 500.0f, "the frame's reticle is found");
        Check(!HudLayerReticleFor(7, &r), "an unknown frame has none");
    }
    // ---- 2: not aiming: the box stays in the panel
    {
        PaintHud(layer);
        HudLayerSetReticle(2, false, nullptr, false, false);
        const HudPanelPlan plan = HudLayerPanelPlan(0, true, true);
        Check(plan.active && plan.panel && !plan.reticle, "plan (left-eye frame, not aiming): the panel only");
        HudPanelTargets t;
        t.panel = panel;
        HudLayerComposite(g_list, eyes, 0, kW, kH, &t);
        Run();
        const auto pp = Read(panel);
        Is(pp, 960, 480, 360, 255, 0, 0, 255, "panel: the reticle's box in the panel");
    }
    // ---- 3: a right-eye frame leaves the panel alone; a frame with no HUD drawn empties it
    {
        PaintHud(layer);
        HudPanelPlan plan = HudLayerPanelPlan(1, true, true);
        Check(plan.active && !plan.panel && !plan.empty, "plan (right-eye frame): the panel as it is");
        HudPanelTargets none;
        HudLayerComposite(g_list, eyes, 1, kW, kH, &none);
        Run();
        D3D12_CPU_DESCRIPTOR_HANDLE ours{};
        HudLayerRedirect(layer, &ours, true);   // a clear, no draw
        plan = HudLayerPanelPlan(0, true, true);   // (the right-eye frame's draw still counts for this one)
        Check(plan.panel, "plan: a HUD draw since the last update counts");
        HudPanelTargets t;
        t.panel = panel;
        HudLayerComposite(g_list, eyes, 0, kW, kH, &t);
        Run();
        Check(t.panelDrawn, "...and the panel is drawn");
        HudLayerRedirect(layer, &ours, true);
        plan = HudLayerPanelPlan(0, true, true);
        Check(plan.active && plan.empty && !plan.panel, "plan: no HUD drawn -> the panel hidden");
        HudLayerComposite(g_list, eyes, 0, kW, kH, &none);
        Run();
        plan = HudLayerPanelPlan(0, true, true);
        Check(plan.empty, "plan: nothing redirected (a menu) -> hidden");
    }
    // ---- 4: the panel not allowed this frame / switched off: the HUD over the eye images, as before
    {
        g_panelSetting = 0.0f;
        PaintHud(layer);
        Grey(eyes[0], eyeRtv[0]);
        Grey(eyes[1], eyeRtv[1]);
        const HudPanelPlan plan = HudLayerPanelPlan(-1, true, true);
        Check(!plan.active && plan.empty, "plan (panel off): not active");
        HudLayerComposite(g_list, eyes, -1, kW, kH, nullptr);
        Run();
        const auto ep = Read(eyes[1]);
        Is(ep, kW, 800, 600, 255, 0, 0, -1, "eye: the HUD's centre over the eye image");
        Is(ep, kW, 400, 400, 0, 255, 0, -1, "eye: the HUD's rectangle over the eye image");
        Is(ep, kW, 150, 150, 255, 255, 0, -1, "eye: the world icon with it");
        Is(ep, kW, 50, 50, 188, 188, 188, -1, "eye: the scene outside the HUD");
        g_panelSetting = 1.0f;
    }

    bool removed = FAILED(g_dev->GetDeviceRemovedReason());
    Check(!removed, "the device is still there");
    ID3D12InfoQueue* iq = nullptr;
    if (debugLayer && SUCCEEDED(g_dev->QueryInterface(IID_PPV_ARGS(&iq))) && iq) {
        const UINT64 n = iq->GetNumStoredMessages();
        int errors = 0;
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            D3D12_MESSAGE* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            iq->GetMessage(i, m, &len);
            if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { ++errors; printf("      D3D12 error: %s\n", m->pDescription); }
        }
        Check(errors == 0, "no D3D12 debug-layer errors");
    }
    printf(g_failed ? "%d FAILED\n" : "all passed (0 failed)\n", g_failed);
    return g_failed ? 1 : 0;
}
