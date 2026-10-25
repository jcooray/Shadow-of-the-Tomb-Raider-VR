#include "build_config.h"
#include "ui_probe.h"

#include <windows.h>
#include <d3d12.h>
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio_cue.h"
#include "camera_interceptor.h"
#include "face_track.h"
#include "hud_layer.h"
#include "logger.h"
#include "present_hook.h"
#include "settings.h"

namespace vrmod {

namespace {

// Brief 2, phase H0 (docs/HUD.md): which draws in a frame are the HUD? A diagnostic, installed only with
// [debug] ui_probe=1. Vtable slots counted from the Windows 10.0.26100.0 SDK d3d12.h (C vtable structs), 2026-09-25:
//   ID3D12Device:            CreateGraphicsPipelineState 10, CreateRenderTargetView 20, CreateDepthStencilView 21
//   ID3D12CommandQueue:      ExecuteCommandLists 10
//   ID3D12GraphicsCommandList: Close 9, Reset 10, DrawInstanced 12, DrawIndexedInstanced 13, RSSetViewports 21,
//                            RSSetScissorRects 22, SetPipelineState 25, OMSetRenderTargets 46, ClearRenderTargetView 48,
//                            BeginEvent 57, EndEvent 58
// Always recorded (both happen at load): each PSO's signature (render-target formats, depth, blending, shader sizes and
// a hash of the pixel shader -- stable across sessions, unlike pointers) and each RTV descriptor's resource size/format.
// Ctrl+Alt+U captures the command lists of the next kCaptureFrames frames (between Presents), per list in submission
// order: passes (render target + viewport + PSO) with their draw counts, and PIX event names if the game emits any.
constexpr int kCaptureFrames = 3;
constexpr size_t kMaxEventsPerList = 20000;

using CreateDevice_t = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateGPS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateRTV_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateDSV_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using Execute_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using Close_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DrawI_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawII_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using Viewports_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using Scissors_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using SetPSO_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using OMSetRT_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ClearRTV_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
using BeginEvent_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const void*, UINT);
using EndEvent_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);

CreateDevice_t oCreateDevice = nullptr;
CreateGPS_t oCreateGPS = nullptr;
CreateRTV_t oCreateRTV = nullptr;
CreateDSV_t oCreateDSV = nullptr;
Execute_t oExecute = nullptr;
std::atomic<UiProbeExecuteFilter> g_executeFilter{nullptr};
Close_t oClose = nullptr;
Reset_t oReset = nullptr;
DrawI_t oDrawI = nullptr;
DrawII_t oDrawII = nullptr;
Viewports_t oViewports = nullptr;
Scissors_t oScissors = nullptr;
SetPSO_t oSetPSO = nullptr;
OMSetRT_t oOMSetRT = nullptr;
ClearRTV_t oClearRTV = nullptr;
BeginEvent_t oBeginEvent = nullptr;
EndEvent_t oEndEvent = nullptr;
std::atomic<bool> g_deviceHooked{ false };

struct PsoSig {
    int id = 0;
    UINT numRT = 0;
    DXGI_FORMAT rt0 = DXGI_FORMAT_UNKNOWN, dsv = DXGI_FORMAT_UNKNOWN;
    bool depth = false, depthWrite = false, blend = false;
    D3D12_BLEND src = D3D12_BLEND_ONE, dst = D3D12_BLEND_ZERO;
    SIZE_T vsSize = 0, psSize = 0;
    uint32_t psHash = 0;
    UINT inputElems = 0;
    bool ui = false;   // the UI signature (H0): 1 RT RGBA8_SRGB, premultiplied alpha, depth off
    void* worldClone = nullptr;   // world-icon pipeline (Scaleform into the HDR scene): its copy for our HUD layer
};
std::atomic<uint64_t> g_worldRestores{ 0 };   // non-icon draws after icon draws in one pass (game state re-bound)
// The Scaleform UI pixel shaders (H0: stable across sessions). The same shaders draw the world icons -- item and
// collectible markers -- into the full-resolution HDR scene target (R11G11B10F, depth-tested, after DLSS; 2026-10-09
// Ctrl+Alt+U captures: 12 more draws of pipelines with PS 0F244A4D / A04BD9F2 with an icon on screen).
bool UiPixelShader(uint32_t h) { return h == 0x0F244A4D || h == 0xA04BD9F2 || h == 0x4C4E4163 || h == 0xD5027F1D; }
std::shared_mutex g_psoMutex;
std::unordered_map<const void*, PsoSig> g_pso;
int g_psoNext = 1;

struct RtvInfo { const void* res; UINT64 w; UINT h; DXGI_FORMAT fmt; };
std::mutex g_rtvMutex;
std::unordered_map<SIZE_T, RtvInfo> g_rtv;
std::mutex g_dsvMutex;
std::unordered_map<SIZE_T, DXGI_FORMAT> g_dsv;   // DSV descriptor -> view format (scene depth D24S8, hair shadow map D16)

// Capture.
std::atomic<int> g_captureLeft{ 0 };        // Presents left in the capture (> 0 = capturing)
std::atomic<bool> g_capturing{ false };
enum EvType : uint8_t { EvPSO, EvRT, EvVP, EvSC, EvDraw, EvClear, EvBegin, EvEnd };
struct Ev {
    EvType t;
    const void* p = nullptr;       // PSO / RT resource
    float a = 0, b = 0, c = 0, d = 0;
    UINT n = 0, m = 0;             // index/vertex count, instance count
    std::string name;
};
std::mutex g_capMutex;
std::unordered_map<const void*, std::vector<Ev>> g_open;    // per command list while recording
std::unordered_map<const void*, std::vector<Ev>> g_closed;  // closed, not yet submitted
struct Submitted { int frame; const void* queue; const void* list; std::vector<Ev> ev; };
std::vector<Submitted> g_submitted;
int g_captureFrame = 0;

// H1b: call stacks of UI-pipeline binds during a capture (which game code draws each Scaleform movie).
struct Stack { void* f[24]; USHORT n; uint32_t count; int firstFrame; };
std::unordered_map<ULONG, Stack> g_stacks;   // keyed by RtlCaptureStackBackTrace's hash

uint32_t Fnv(const void* p, SIZE_T n) {
    uint32_t h = 2166136261u;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (SIZE_T i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}

// ---- H1: the UI drawn smaller and centred (docs/HUD.md) ----
// While a UI pipeline (PsoSig::ui) draws into an sRGB target, its viewport and scissor rectangles are mapped about the
// target's centre: p' = c + (p - c) * scale, plus offset_y * height. The game's own values are kept per command list
// so switching to a non-UI pipeline puts them back. Menus and the map are left alone.
std::atomic<bool> g_hudOn{ false };
std::atomic<float> g_hudScale{ 0.6f }, g_hudOffY{ 0.0f };
std::atomic<float> g_hudAimX{ 0.0f }, g_hudAimY{ 0.0f };   // UiProbeSetAimOffset (NDC)
std::atomic<uint32_t> g_hudRewrites{ 0 };

struct ListState {
    const void* list = nullptr;
    bool ui = false;          // a UI pipeline is bound
    bool srgb = false;        // render target 0 is an sRGB RGBA8 target (the UI layer)
    float rtW = 0, rtH = 0;
    bool haveVp = false;
    D3D12_VIEWPORT vp{};
    UINT nsc = 0;
    D3D12_RECT sc[4]{};
    bool modified = false;    // we issued scaled values on this list (so the game's must be put back later)
};
thread_local ListState t_lists[8];
thread_local int t_next = 0;

ListState& St(const void* cl) {
    for (ListState& s : t_lists) if (s.list == cl) return s;
    ListState& s = t_lists[t_next];
    t_next = (t_next + 1) % 8;
    s = ListState{};
    s.list = cl;
    return s;
}

bool HudActive() {
    return g_hudOn.load(std::memory_order_relaxed) && !MenuScreenActive();
}

bool IsUiPso(const void* pso) {
    std::shared_lock<std::shared_mutex> lock(g_psoMutex);
    auto it = g_pso.find(pso);
    return it != g_pso.end() && it->second.ui;
}

D3D12_VIEWPORT ScaleVp(const D3D12_VIEWPORT& v, const ListState& st) {
    const float s = g_hudScale.load(std::memory_order_relaxed);
    const float cx = st.rtW * 0.5f, cy = st.rtH * 0.5f;
    const float oy = g_hudOffY.load(std::memory_order_relaxed) * st.rtH - g_hudAimY.load(std::memory_order_relaxed) * st.rtH * 0.5f;
    const float ox = g_hudAimX.load(std::memory_order_relaxed) * st.rtW * 0.5f;
    D3D12_VIEWPORT o = v;
    o.TopLeftX = cx + (v.TopLeftX - cx) * s + ox;
    o.TopLeftY = cy + (v.TopLeftY - cy) * s + oy;
    o.Width = v.Width * s;
    o.Height = v.Height * s;
    return o;
}

D3D12_RECT ScaleRect(const D3D12_RECT& r, const ListState& st) {
    const float s = g_hudScale.load(std::memory_order_relaxed);
    const float cx = st.rtW * 0.5f, cy = st.rtH * 0.5f;
    const float oy = g_hudOffY.load(std::memory_order_relaxed) * st.rtH - g_hudAimY.load(std::memory_order_relaxed) * st.rtH * 0.5f;
    const float ox = g_hudAimX.load(std::memory_order_relaxed) * st.rtW * 0.5f;
    D3D12_RECT o;
    o.left = static_cast<LONG>(std::floor(cx + (r.left - cx) * s + ox));
    o.top = static_cast<LONG>(std::floor(cy + (r.top - cy) * s + oy));
    o.right = static_cast<LONG>(std::ceil(cx + (r.right - cx) * s + ox));
    o.bottom = static_cast<LONG>(std::ceil(cy + (r.bottom - cy) * s + oy));
    return o;
}

bool Rewriting(const ListState& st) {
    return st.ui && st.srgb && st.rtW > 0.0f;
}

// Issues the list's current viewport/scissor again: scaled while rewriting, the game's own otherwise (only needed if
// we changed them earlier on this list).
void Reissue(ID3D12GraphicsCommandList* cl, ListState& st) {
    const bool rw = Rewriting(st);
    if (!rw && !st.modified) return;
    D3D12_VIEWPORT v = st.haveVp ? st.vp : D3D12_VIEWPORT{ 0.0f, 0.0f, st.rtW, st.rtH, 0.0f, 1.0f };
    if (rw) v = ScaleVp(v, st);
    oViewports(cl, 1, &v);
    D3D12_RECT r[4];
    UINT n = st.nsc;
    if (n == 0) {
        n = 1;
        r[0] = D3D12_RECT{ 0, 0, static_cast<LONG>(st.rtW), static_cast<LONG>(st.rtH) };
        if (rw) r[0] = ScaleRect(r[0], st);
    } else {
        for (UINT i = 0; i < n; ++i) r[i] = rw ? ScaleRect(st.sc[i], st) : st.sc[i];
    }
    oScissors(cl, n, r);
    st.modified = rw;
    if (rw) g_hudRewrites.fetch_add(1, std::memory_order_relaxed);
}

void Record(ID3D12GraphicsCommandList* cl, Ev&& e) {
    std::lock_guard<std::mutex> lock(g_capMutex);
    auto& v = g_open[cl];
    if (v.size() < kMaxEventsPerList) v.push_back(std::move(e));
}

// ---- hooks ----
HRESULT STDMETHODCALLTYPE hkCreateGPS(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** out) {
    const HRESULT hr = oCreateGPS(dev, d, riid, out);
    if (SUCCEEDED(hr) && d && out && *out) {
        PsoSig s;
        s.numRT = d->NumRenderTargets;
        s.rt0 = d->RTVFormats[0];
        s.dsv = d->DSVFormat;
        s.depth = d->DepthStencilState.DepthEnable != FALSE;
        s.depthWrite = d->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
        s.blend = d->BlendState.RenderTarget[0].BlendEnable != FALSE;
        s.src = d->BlendState.RenderTarget[0].SrcBlend;
        s.dst = d->BlendState.RenderTarget[0].DestBlend;
        s.vsSize = d->VS.BytecodeLength;
        s.psSize = d->PS.BytecodeLength;
        s.psHash = d->PS.pShaderBytecode ? Fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength) : 0;
        s.inputElems = d->InputLayout.NumElements;
        s.ui = s.numRT == 1 && s.rt0 == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && !s.depth && s.blend && s.src == D3D12_BLEND_ONE
            && s.dst == D3D12_BLEND_INV_SRC_ALPHA;
        // World icons: a copy for our HUD layer (RGBA8 sRGB, alpha kept as coverage), same depth test against the scene.
        if (s.numRT == 1 && s.rt0 == DXGI_FORMAT_R11G11B10_FLOAT && s.blend && s.src == D3D12_BLEND_ONE
            && s.dst == D3D12_BLEND_INV_SRC_ALPHA && UiPixelShader(s.psHash)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC c = *d;
            c.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            D3D12_RENDER_TARGET_BLEND_DESC& b = c.BlendState.RenderTarget[0];
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            void* clone = nullptr;
            if (SUCCEEDED(oCreateGPS(dev, &c, riid, &clone))) s.worldClone = clone;
        }
        std::lock_guard<std::shared_mutex> lock(g_psoMutex);
        s.id = g_psoNext++;
        g_pso[*out] = s;
    }
    return hr;
}

void STDMETHODCALLTYPE hkCreateDSV(ID3D12Device* dev, ID3D12Resource* res, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    oCreateDSV(dev, res, d, h);
    if (res || d) {
        const DXGI_FORMAT f = d ? d->Format : res->GetDesc().Format;
        std::lock_guard<std::mutex> lock(g_dsvMutex);
        g_dsv[h.ptr] = f;
    }
}

void STDMETHODCALLTYPE hkCreateRTV(ID3D12Device* dev, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    oCreateRTV(dev, res, d, h);
    if (res) {
        const D3D12_RESOURCE_DESC rd = res->GetDesc();
        std::lock_guard<std::mutex> lock(g_rtvMutex);
        g_rtv[h.ptr] = { res, rd.Width, rd.Height, d ? d->Format : rd.Format };
    }
}

bool SkipHeadDraw(const void* cl, UINT indexCount, UINT startIndex);
bool SkipHairComposite(const void* cl, UINT vertexCount);
void NoteRt0(const void* cl, const void* res, bool srgb);
void NoteUiPso(const void* cl, bool ui);
void SeeUiDraw(const void* cl);
void NotePso(const void* cl, const void* pso, void* clone);
void NoteTargets(const void* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv, bool hdr);
bool WorldIconDrawI(ID3D12GraphicsCommandList* cl, UINT v, UINT inst, UINT sv, UINT si);
bool WorldIconDrawII(ID3D12GraphicsCommandList* cl, UINT idx, UINT inst, UINT si, INT bv, UINT sInst);
void NoteRenderTargets(const void* cl, UINT n);
void NoteDepthTarget(const void* cl, DXGI_FORMAT f);
void ForgetList(const void* cl);

void STDMETHODCALLTYPE hkExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (g_capturing.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_capMutex);
        for (UINT i = 0; i < n; ++i) {
            auto it = g_closed.find(lists[i]);
            Submitted s{ g_captureFrame, q, lists[i], {} };
            if (it != g_closed.end()) { s.ev = std::move(it->second); g_closed.erase(it); }
            g_submitted.push_back(std::move(s));
        }
    }
    if (UiProbeExecuteFilter f = g_executeFilter.load(std::memory_order_relaxed)) {
        if (f(q, n, lists, oExecute)) return;
    }
    oExecute(q, n, lists);
}

HRESULT STDMETHODCALLTYPE hkReset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* a, ID3D12PipelineState* pso) {
    ForgetList(cl);
    if (g_capturing.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_capMutex);
        g_open[cl].clear();
        if (pso) g_open[cl].push_back(Ev{ EvPSO, pso });
    }
    if (g_hudOn.load(std::memory_order_relaxed)) {
        ListState& st = St(cl);
        st = ListState{};
        st.list = cl;
        st.ui = pso && IsUiPso(pso);
    }
    return oReset(cl, a, pso);
}

HRESULT STDMETHODCALLTYPE hkClose(ID3D12GraphicsCommandList* cl) {
    if (g_capturing.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_capMutex);
        auto it = g_open.find(cl);
        if (it != g_open.end()) { g_closed[cl] = std::move(it->second); g_open.erase(it); }
    }
    return oClose(cl);
}

void STDMETHODCALLTYPE hkDrawI(ID3D12GraphicsCommandList* cl, UINT v, UINT inst, UINT sv, UINT si) {
    if (SkipHairComposite(cl, v)) return;
    SeeUiDraw(cl);
    if (WorldIconDrawI(cl, v, inst, sv, si)) return;
    if (g_capturing.load(std::memory_order_relaxed)) { Ev e{ EvDraw }; e.n = v; e.m = inst; Record(cl, std::move(e)); }
    oDrawI(cl, v, inst, sv, si);
}

void STDMETHODCALLTYPE hkDrawII(ID3D12GraphicsCommandList* cl, UINT idx, UINT inst, UINT si, INT bv, UINT sInst) {
    SeeUiDraw(cl);
    if (SkipHeadDraw(cl, idx, si)) return;
    if (WorldIconDrawII(cl, idx, inst, si, bv, sInst)) return;
    if (g_capturing.load(std::memory_order_relaxed)) { Ev e{ EvDraw }; e.n = idx; e.m = inst; e.a = 1; Record(cl, std::move(e)); }
    oDrawII(cl, idx, inst, si, bv, sInst);
}

void STDMETHODCALLTYPE hkViewports(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VIEWPORT* vp) {
    if (g_capturing.load(std::memory_order_relaxed) && n > 0 && vp) {
        Ev e{ EvVP }; e.a = vp[0].TopLeftX; e.b = vp[0].TopLeftY; e.c = vp[0].Width; e.d = vp[0].Height; Record(cl, std::move(e));
    }
    if (n == 1 && vp && HudActive()) {
        ListState& st = St(cl);
        st.vp = vp[0];
        st.haveVp = true;
        if (Rewriting(st)) {
            const D3D12_VIEWPORT v = ScaleVp(vp[0], st);
            st.modified = true;
            g_hudRewrites.fetch_add(1, std::memory_order_relaxed);
            oViewports(cl, 1, &v);
            return;
        }
    }
    oViewports(cl, n, vp);
}

void STDMETHODCALLTYPE hkScissors(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RECT* r) {
    if (g_capturing.load(std::memory_order_relaxed) && n > 0 && r) {
        Ev e{ EvSC };
        e.a = static_cast<float>(r[0].left); e.b = static_cast<float>(r[0].top);
        e.c = static_cast<float>(r[0].right - r[0].left); e.d = static_cast<float>(r[0].bottom - r[0].top);
        Record(cl, std::move(e));
    }
    if (n >= 1 && n <= 4 && r && HudActive()) {
        ListState& st = St(cl);
        st.nsc = n;
        for (UINT i = 0; i < n; ++i) st.sc[i] = r[i];
        if (Rewriting(st)) {
            D3D12_RECT o[4];
            for (UINT i = 0; i < n; ++i) o[i] = ScaleRect(r[i], st);
            st.modified = true;
            oScissors(cl, n, o);
            return;
        }
    }
    oScissors(cl, n, r);
}

void STDMETHODCALLTYPE hkSetPSO(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso) {
    if (g_capturing.load(std::memory_order_relaxed)) {
        Ev e{ EvPSO, pso };
        Record(cl, std::move(e));
        if (pso && IsUiPso(pso)) {
            Stack st{};
            ULONG hash = 0;
            st.n = RtlCaptureStackBackTrace(1, 24, st.f, &hash);
            std::lock_guard<std::mutex> lock(g_capMutex);
            auto it = g_stacks.find(hash);
            if (it == g_stacks.end()) { st.count = 1; st.firstFrame = g_captureFrame; g_stacks[hash] = st; }
            else ++it->second.count;
        }
    }
    oSetPSO(cl, pso);
    {
        bool ui = false;
        void* clone = nullptr;
        if (pso) {
            std::shared_lock<std::shared_mutex> lock(g_psoMutex);
            auto it = g_pso.find(pso);
            if (it != g_pso.end()) { ui = it->second.ui; clone = it->second.worldClone; }
        }
        NoteUiPso(cl, ui);
        NotePso(cl, pso, clone);
    }
    if (HudActive()) {
        ListState& st = St(cl);
        const bool ui = pso && IsUiPso(pso);
        if (ui != st.ui) {
            st.ui = ui;
            Reissue(cl, st);
        }
    }
}

void STDMETHODCALLTYPE hkOMSetRT(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (g_capturing.load(std::memory_order_relaxed)) {
        Ev e{ EvRT };
        e.n = n;
        e.m = dsv ? 1 : 0;
        if (n > 0 && rts) {
            std::lock_guard<std::mutex> lock(g_rtvMutex);
            auto it = g_rtv.find(rts[0].ptr);
            if (it != g_rtv.end()) { e.p = it->second.res; e.a = static_cast<float>(it->second.w); e.b = static_cast<float>(it->second.h); e.c = static_cast<float>(it->second.fmt); }
        }
        Record(cl, std::move(e));
    }
    // HUD layer (hud_layer.cpp): binding the game's HUD layer binds ours instead.
    const void* res0 = nullptr;
    bool srgb0 = false;
    if (n > 0 && rts) {
        std::lock_guard<std::mutex> lock(g_rtvMutex);
        auto it = g_rtv.find(rts[0].ptr);
        if (it != g_rtv.end()) { res0 = it->second.res; srgb0 = it->second.fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE ours{};
    if (n == 1 && srgb0 && HudLayerRedirect(res0, &ours)) oOMSetRT(cl, n, &ours, single, dsv);
    else oOMSetRT(cl, n, rts, single, dsv);
    NoteRenderTargets(cl, n);
    NoteRt0(cl, res0, srgb0);
    {
        bool hdr0 = false;
        if (n > 0 && rts) {
            std::lock_guard<std::mutex> lock(g_rtvMutex);
            auto it = g_rtv.find(rts[0].ptr);
            hdr0 = it != g_rtv.end() && it->second.fmt == DXGI_FORMAT_R11G11B10_FLOAT;
        }
        NoteTargets(cl, n, rts, single, dsv, hdr0);
    }
    {
        DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
        if (dsv) {
            std::lock_guard<std::mutex> lock(g_dsvMutex);
            auto it = g_dsv.find(dsv->ptr);
            if (it != g_dsv.end()) f = it->second;
        }
        NoteDepthTarget(cl, f);
    }
    if (HudActive()) {
        ListState& st = St(cl);
        st.srgb = false;
        st.rtW = st.rtH = 0.0f;
        if (n > 0 && rts) {
            std::lock_guard<std::mutex> lock(g_rtvMutex);
            auto it = g_rtv.find(rts[0].ptr);
            if (it != g_rtv.end()) {
                st.srgb = it->second.fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                st.rtW = static_cast<float>(it->second.w);
                st.rtH = static_cast<float>(it->second.h);
            }
        }
        Reissue(cl, st);
    }
}

void STDMETHODCALLTYPE hkClearRTV(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* color, UINT n, const D3D12_RECT* r) {
    if (g_capturing.load(std::memory_order_relaxed)) {
        Ev e{ EvClear };
        std::lock_guard<std::mutex> lock(g_rtvMutex);
        auto it = g_rtv.find(h.ptr);
        if (it != g_rtv.end()) { e.p = it->second.res; e.a = static_cast<float>(it->second.w); e.b = static_cast<float>(it->second.h); }
        Record(cl, std::move(e));
    }
    oClearRTV(cl, h, color, n, r);
    // HUD layer: the game clearing its layer clears ours too (its layer itself stays empty: nothing is drawn into it).
    const void* res = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_rtvMutex);
        auto it = g_rtv.find(h.ptr);
        if (it != g_rtv.end()) res = it->second.res;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE ours{};
    if (res && HudLayerRedirect(res, &ours)) oClearRTV(cl, ours, color, n, r);
}

std::string EventName(UINT meta, const void* data, UINT size) {
    if (!data || size == 0) return "";
    std::string s;
    if (meta == 0) {   // PIX_EVENT_UNICODE_VERSION: wide string
        const wchar_t* w = static_cast<const wchar_t*>(data);
        for (UINT i = 0; i < size / 2 && w[i]; ++i) s += (w[i] < 128) ? static_cast<char>(w[i]) : '?';
    } else {           // ANSI (1) or a PIX blob (2): keep the printable run(s)
        const char* c = static_cast<const char*>(data);
        for (UINT i = 0; i < size; ++i) if (c[i] >= 32 && c[i] < 127) s += c[i]; else if (!s.empty() && s.back() != ' ') s += ' ';
    }
    if (s.size() > 80) s.resize(80);
    return s;
}

void STDMETHODCALLTYPE hkBeginEvent(ID3D12GraphicsCommandList* cl, UINT meta, const void* data, UINT size) {
    if (g_capturing.load(std::memory_order_relaxed)) { Ev e{ EvBegin }; e.name = EventName(meta, data, size); Record(cl, std::move(e)); }
    oBeginEvent(cl, meta, data, size);
}

void STDMETHODCALLTYPE hkEndEvent(ID3D12GraphicsCommandList* cl) {
    if (g_capturing.load(std::memory_order_relaxed)) { Ev e{ EvEnd }; Record(cl, std::move(e)); }
    oEndEvent(cl);
}

// ---- Head hiding (docs/FIRSTPERSON.md, 2026-10-07 RenderDoc capture) ----
// Lara's head is its own model: every face, eye, mouth and hair-card draw uses one index buffer (the frame capture's
// Buffer 129358), separate from her body's. In first person with the head camera, every indexed draw from that buffer
// into colour targets is skipped (the depth-only shadow-map draws stay, so her shadow keeps its head). The buffer is
// learned each frame from the face draw's signature -- 71256 indices from index 0 (the head at LOD 0, which it always is
// with the camera inside it) -- so a reload that moves the buffer is followed.
// Hide groups (2026-10-07): each learned from one draw's signature (index count, first index) in a colour pass, then
// every indexed draw from the same index buffer -- or, with a part list, only those parts -- is skipped while the group
// is active. Head: the face draw (71256 from 0); bow: 7962 from 1158; quiver: 14898 from 12300 (capture Buffers 129358,
// 122530, 129782). Gear finder (Ctrl+Alt+F6): the models drawn just before and after the head's face draw (Lara's own
// draws come as one block in the G-buffer pass) are listed, and one at a time can be hidden -- for finding the bow and
// quiver models of other levels / outfits / bows, whose signatures then go into kSignatures.
struct CensusRec { uint64_t ibVa; UINT count, first; };
constexpr int kCensusBefore = 32, kCensusAfter = 16;
struct DrawState {
    const void* list = nullptr; uint64_t ibVa = 0; UINT numRT = 0; bool ib32 = false; bool hairSkipped = false; DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    const void* rt0 = nullptr; bool rt0srgb = false; bool ui = false;   // HUD layer learning
    const void* pso = nullptr; void* worldClone = nullptr; bool rt0hdr = false;   // world icons -> HUD layer
    UINT nrt = 0; BOOL single = FALSE; D3D12_CPU_DESCRIPTOR_HANDLE rts[8] = {}; bool haveDsv = false; D3D12_CPU_DESCRIPTOR_HANDLE dsvH = {};
    bool inOurs = false;   // our layer + a clone are bound (left so for following icon draws; restored before anything else)
    CensusRec ring[kCensusBefore]; int ringN = 0; int after = -1;   // gear-finder census (only while one is wanted)
};
thread_local DrawState t_draw[8];
thread_local int t_drawNext = 0;

struct Part { UINT count, first; };
// Strand hair (TressFX): its own 32-bit index buffer (capture Buffer 136398), 334884 indices from 0, drawn twice (full and
// half resolution) before the full-screen hair composite; found 2026-10-07 after the head model was hidden.
// Body: Lara's body/outfit model (38865 from 57927) -- kept out of the gear finder's list; hidden only while the first-person
// view points well away from where her body faces (head_drive.cpp, hide_body_deg; 2026-10-10).
enum Group { kHead, kBow, kQuiver, kBody, kHair, kGroups };
struct Signature { int group; Part part; };
const Signature kSignatures[] = {
    { kHead, { 71256, 0 } }, { kBow, { 7962, 1158 } }, { kQuiver, { 14898, 12300 } }, { kBody, { 38865, 57927 } },
    { kBow, { 9894, 1908 } },   // second level's bow (gear finder 2026-10-07: 1908@0, 9894@1908, 1401@11802)
    { kBow, { 10998, 20928 } },   // third bow (gear finder 2026-10-07 23:43: 20928@0, 10998@20928)
};
const char* const kGroupName[kGroups] = { "head", "bow", "quiver", "body", "strand hair" };
std::atomic<uint64_t> g_groupIb[kGroups];
std::atomic<bool> g_groupOn[kGroups];

// Gear finder.
struct GearCandidate { uint64_t ibVa; std::vector<Part> parts; };
std::mutex g_censusMutex;
std::vector<GearCandidate> g_candidates;
std::atomic<bool> g_censusWant{ false };
std::atomic<bool> g_censusDone{ false };
std::atomic<uint64_t> g_gearTestIb{ 0 };   // the candidate hidden now (0 = none)
int g_gearTestIndex = 0;                   // 1-based; 0 = off (hotkey thread only)

void CensusFinish(const DrawState& d, const std::vector<CensusRec>& after) {
    std::vector<CensusRec> all(d.ring, d.ring + d.ringN);
    all.insert(all.end(), after.begin(), after.end());
    std::vector<GearCandidate> c;
    for (const CensusRec& r : all) {
        if (r.ibVa == g_groupIb[kHead].load() || r.ibVa == g_groupIb[kHair].load() || r.ibVa == g_groupIb[kBody].load()) continue;
        auto it = std::find_if(c.begin(), c.end(), [&](const GearCandidate& g) { return g.ibVa == r.ibVa; });
        if (it == c.end()) { c.push_back({ r.ibVa, {} }); it = c.end() - 1; }
        const bool dup = std::any_of(it->parts.begin(), it->parts.end(), [&](const Part& q) { return q.count == r.count && q.first == r.first; });
        if (!dup) it->parts.push_back({ r.count, r.first });
    }
    std::lock_guard<std::mutex> lock(g_censusMutex);
    g_candidates = std::move(c);
    g_censusDone.store(true);
}
std::atomic<uint64_t> g_groupSkipped[kGroups], g_groupLearned[kGroups];
// Body census (Ctrl+Alt+F12, 2026-10-12): every colour-pass indexed draw over 3 frames with the constants it uses
// (root table param 2, b0 WorldBuffer and b1 CB_Skinning addresses), the face draw marked -- do her body parts share
// the face's constants (an outfit-independent way to hide her body)? Written to vrmod_bodycensus_<time>.txt.
struct CensusDraw { int frame; const void* list; uint64_t ib; UINT count, first; uint64_t table, b0, b1; bool face; };
std::mutex g_bodyCensusMutex;
std::vector<CensusDraw> g_bodyCensus;
std::atomic<int> g_bodyCensusFrame{ -1 };   // -1 off; 0..2 the frame being recorded
std::atomic<UINT> g_hairCount{ 0 };   // the strand-hair draw's index count last seen (log)
std::atomic<uint64_t> g_hairDepthSkipped{ 0 };
std::atomic<uint64_t> g_hairDepthKept{ 0 };   // last depth-only strand draw left alone: DSV format << 32 | index count (log)

DrawState& Ds(const void* cl) {
    for (DrawState& d : t_draw) if (d.list == cl) return d;
    DrawState& d = t_draw[t_drawNext];
    t_drawNext = (t_drawNext + 1) % 8;
    d = DrawState{};
    d.list = cl;
    return d;
}

using IASetIB_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
IASetIB_t oIASetIB = nullptr;
void STDMETHODCALLTYPE hkIASetIB(ID3D12GraphicsCommandList* cl, const D3D12_INDEX_BUFFER_VIEW* v) {
    DrawState& d = Ds(cl);
    d.ibVa = v ? v->BufferLocation : 0;
    d.ib32 = v && v->Format == DXGI_FORMAT_R32_UINT;
    oIASetIB(cl, v);
}

void NoteRenderTargets(const void* cl, UINT n) { Ds(cl).numRT = n; }
void NoteDepthTarget(const void* cl, DXGI_FORMAT f) { Ds(cl).dsv = f; }
void NoteRt0(const void* cl, const void* res, bool srgb) { DrawState& d = Ds(cl); d.rt0 = res; d.rt0srgb = srgb; }
void NoteUiPso(const void* cl, bool ui) { Ds(cl).ui = ui; }
void NotePso(const void* cl, const void* pso, void* clone) { DrawState& d = Ds(cl); d.pso = pso; d.worldClone = clone; }
void NoteTargets(const void* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv, bool hdr) {
    DrawState& d = Ds(cl);
    d.nrt = (std::min)(n, 8u);
    d.single = single;
    const UINT copy = single ? (std::min)(d.nrt, 1u) : d.nrt;
    for (UINT i = 0; i < copy && rts; ++i) d.rts[i] = rts[i];
    d.haveDsv = dsv != nullptr;
    if (dsv) d.dsvH = *dsv;
    d.rt0hdr = hdr;
    d.inOurs = false;   // the game bound its own targets
}
// A world-icon draw: into our HUD layer with the cloned pipeline (same depth buffer). Our target stays bound for the
// following icon draws (the pass holds nothing else in the captures); the game's targets and pipeline are put back
// only if another kind of draw follows before the game binds its own targets -- re-binding the game's RTV handles
// right away could pick up descriptor slots it has reused since.
template <typename F>
bool WorldIconDraw(ID3D12GraphicsCommandList* cl, F draw) {
    DrawState& d = Ds(cl);
    D3D12_CPU_DESCRIPTOR_HANDLE ours{};
    if (d.worldClone && d.rt0hdr && d.nrt == 1 && HudLayerWorldRtv(&ours)) {
        if (!d.inOurs) {
            oOMSetRT(cl, 1, &ours, FALSE, d.haveDsv ? &d.dsvH : nullptr);
            d.inOurs = true;
        }
        oSetPSO(cl, static_cast<ID3D12PipelineState*>(d.worldClone));
        draw();
        return true;
    }
    if (d.inOurs) {   // something else follows in the same pass: the game's state back first
        oOMSetRT(cl, d.nrt, d.rts, d.single, d.haveDsv ? &d.dsvH : nullptr);
        if (d.pso) oSetPSO(cl, static_cast<ID3D12PipelineState*>(const_cast<void*>(d.pso)));
        d.inOurs = false;
        g_worldRestores.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}
// A UI-pipeline draw into an sRGB RGBA8 target: that target is the game's HUD layer (docs/HUD.md, H0).
void SeeUiDraw(const void* cl) {
    const DrawState& d = Ds(cl);
    if (d.ui && d.rt0srgb && d.rt0) HudLayerSeeUiDraw(static_cast<ID3D12Resource*>(const_cast<void*>(d.rt0)));
}
void ForgetList(const void* cl) { DrawState& d = Ds(cl); d.ibVa = 0; d.numRT = 0; d.hairSkipped = false; d.dsv = DXGI_FORMAT_UNKNOWN; d.ringN = 0; d.after = -1; d.rt0 = nullptr; d.rt0srgb = false; d.ui = false; d.pso = nullptr; d.worldClone = nullptr; d.rt0hdr = false; d.nrt = 0; d.haveDsv = false; FaceTrackForgetList(cl); }

// True when this indexed draw belongs to an active hide group and should be skipped.
bool SkipHeadDraw(const void* cl, UINT indexCount, UINT startIndex) {
    DrawState& d = Ds(cl);
    if (d.ibVa == 0 || indexCount < 24) return false;
    if (d.numRT == 0) {
        // Depth-only: shadow maps keep everything (Lara's shadow keeps its head), except the strand hair's depth pass into
        // the scene depth (capture event 19215). Left in, fog, lighting and the rest saw a see-through hair imprint. Told
        // apart from its shadow-map draws by the depth format -- the scene depth has stencil (D24S8), the hair shadow map
        // is D16 -- since the strand count changes (after a cutscene the depth pass stopped matching the colour count).
        if (d.ibVa != g_groupIb[kHair].load(std::memory_order_relaxed) || !g_groupOn[kHair].load(std::memory_order_relaxed)) return false;
        if (d.dsv == DXGI_FORMAT_D24_UNORM_S8_UINT || d.dsv == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
            g_hairDepthSkipped.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        g_hairDepthKept.store((static_cast<uint64_t>(d.dsv) << 32) | indexCount, std::memory_order_relaxed);
        return false;
    }
    if (g_censusWant.load(std::memory_order_relaxed)) {
        // Gear finder census: keep the last kCensusBefore colour draws of this list; at the head's face draw, collect the
        // next kCensusAfter and hand the lot over.
        static thread_local std::vector<CensusRec> after;
        if (d.after >= 0) {
            after.push_back({ d.ibVa, indexCount, startIndex });
            if (--d.after == 0) {
                d.after = -1;
                if (g_censusWant.exchange(false)) CensusFinish(d, after);
                after.clear();
            }
        } else if (indexCount == 71256 && startIndex == 0) {
            d.after = kCensusAfter;
            after.clear();
        } else {
            if (d.ringN == kCensusBefore) { std::memmove(d.ring, d.ring + 1, sizeof(CensusRec) * (kCensusBefore - 1)); --d.ringN; }
            d.ring[d.ringN++] = { d.ibVa, indexCount, startIndex };
        }
    }
    if (indexCount == 71256 && startIndex == 0) FaceTrackOnFaceDraw(cl);   // the head lock reads her head from this draw
    if (FaceTrackBodyLearning() && indexCount >= 300 && d.ibVa != g_groupIb[kHead].load(std::memory_order_relaxed))
        FaceTrackLearnDraw(cl, d.ibVa, indexCount, startIndex);   // which draws are her body (any outfit)
    if (const int cf = g_bodyCensusFrame.load(std::memory_order_relaxed); cf >= 0) {
        CensusDraw c{ cf, cl, d.ibVa, indexCount, startIndex, 0, 0, 0, indexCount == 71256 && startIndex == 0 };
        FaceTrackDrawKeys(cl, &c.table, &c.b0, &c.b1);
        std::lock_guard<std::mutex> lock(g_bodyCensusMutex);
        if (g_bodyCensus.size() < 60000) g_bodyCensus.push_back(c);
    }
    if (d.ibVa == g_gearTestIb.load(std::memory_order_relaxed)) return true;
    // Strand hair: the count varies (334884 in the Photo Mode capture; not seen at that count in VR play), so it's learned
    // from any colour-pass draw of 150000+ 32-bit indices from index 0 -- by far the largest draws in the frame.
    if (d.ib32 && startIndex == 0 && indexCount >= 150000) {
        g_hairCount.store(indexCount, std::memory_order_relaxed);
        if (d.ibVa != g_groupIb[kHair].load(std::memory_order_relaxed)) {
            g_groupIb[kHair].store(d.ibVa, std::memory_order_relaxed);
            g_groupLearned[kHair].fetch_add(1, std::memory_order_relaxed);
        }
    }
    for (const Signature& sig : kSignatures) {
        if (indexCount == sig.part.count && startIndex == sig.part.first && d.ibVa != g_groupIb[sig.group].load(std::memory_order_relaxed)) {
            g_groupIb[sig.group].store(d.ibVa, std::memory_order_relaxed);
            g_groupLearned[sig.group].fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Her body, by the learned index buffers (face_track.cpp body learning; any outfit).
    if (g_groupOn[kBody].load(std::memory_order_relaxed) && FaceTrackIsBodyIb(d.ibVa)) {
        g_groupSkipped[kBody].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    for (int g = 0; g < kGroups; ++g) {
        if (d.ibVa != g_groupIb[g].load(std::memory_order_relaxed)) continue;
        if (!g_groupOn[g].load(std::memory_order_relaxed)) continue;
        g_groupSkipped[g].fetch_add(1, std::memory_order_relaxed);
        if (g == kHair) d.hairSkipped = true;
        return true;
    }
    return false;
}

// The strand-hair composite: the first full-screen triangle (DrawInstanced of 3 vertices) in a colour pass after a skipped
// strand draw in the same command list (capture event 19317: blends the hair targets -- R8G8B8A8 SNORM/UNORM and the
// half-resolution R8 coverage -- over the HDR target, stencil-masked to Lara). The hair targets are only discarded each
// frame, never cleared, so with the strands skipped it would re-blend stale hair; skip it too.
std::atomic<uint64_t> g_hairCompositeSkipped{ 0 };
bool SkipHairComposite(const void* cl, UINT vertexCount) {
    DrawState& d = Ds(cl);
    if (!d.hairSkipped || vertexCount != 3 || d.numRT == 0) return false;
    d.hairSkipped = false;
    g_hairCompositeSkipped.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool Hook(void* target, void* detour, void** orig, const char* name) {
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        modlog::Line(std::string("UIPROBE: hook failed: ") + name);
        return false;
    }
    return true;
}

void HookDevice(ID3D12Device* dev) {
    if (g_deviceHooked.exchange(true)) return;
    void** dv = *reinterpret_cast<void***>(dev);
    Hook(dv[10], reinterpret_cast<void*>(&hkCreateGPS), reinterpret_cast<void**>(&oCreateGPS), "CreateGraphicsPipelineState");
    Hook(dv[20], reinterpret_cast<void*>(&hkCreateRTV), reinterpret_cast<void**>(&oCreateRTV), "CreateRenderTargetView");
    Hook(dv[21], reinterpret_cast<void*>(&hkCreateDSV), reinterpret_cast<void**>(&oCreateDSV), "CreateDepthStencilView");   // d3d12.h: 21
    // Queue and command-list vtables from temporary objects (the functions are shared by all instances).
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))
        && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))
        && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&cl)))) {
        void** qv = *reinterpret_cast<void***>(q);
        void** lv = *reinterpret_cast<void***>(cl);
        Hook(qv[10], reinterpret_cast<void*>(&hkExecute), reinterpret_cast<void**>(&oExecute), "ExecuteCommandLists");
        Hook(lv[9], reinterpret_cast<void*>(&hkClose), reinterpret_cast<void**>(&oClose), "Close");
        Hook(lv[10], reinterpret_cast<void*>(&hkReset), reinterpret_cast<void**>(&oReset), "Reset");
        Hook(lv[12], reinterpret_cast<void*>(&hkDrawI), reinterpret_cast<void**>(&oDrawI), "DrawInstanced");
        Hook(lv[13], reinterpret_cast<void*>(&hkDrawII), reinterpret_cast<void**>(&oDrawII), "DrawIndexedInstanced");
        Hook(lv[21], reinterpret_cast<void*>(&hkViewports), reinterpret_cast<void**>(&oViewports), "RSSetViewports");
        Hook(lv[22], reinterpret_cast<void*>(&hkScissors), reinterpret_cast<void**>(&oScissors), "RSSetScissorRects");
        Hook(lv[25], reinterpret_cast<void*>(&hkSetPSO), reinterpret_cast<void**>(&oSetPSO), "SetPipelineState");
        Hook(lv[46], reinterpret_cast<void*>(&hkOMSetRT), reinterpret_cast<void**>(&oOMSetRT), "OMSetRenderTargets");
        Hook(lv[43], reinterpret_cast<void*>(&hkIASetIB), reinterpret_cast<void**>(&oIASetIB), "IASetIndexBuffer");   // d3d12.h vtable: 43
        Hook(lv[48], reinterpret_cast<void*>(&hkClearRTV), reinterpret_cast<void**>(&oClearRTV), "ClearRenderTargetView");
        Hook(lv[57], reinterpret_cast<void*>(&hkBeginEvent), reinterpret_cast<void**>(&oBeginEvent), "BeginEvent");
        Hook(lv[58], reinterpret_cast<void*>(&hkEndEvent), reinterpret_cast<void**>(&oEndEvent), "EndEvent");
        FaceTrackInstall(dev, dv, lv);
        cl->Close();
    }
    if (cl) cl->Release();
    if (a) a->Release();
    if (q) q->Release();
    modlog::Line("UIPROBE: hooks installed on the game's D3D12 device (PSO/RTV creation recorded from now on; Ctrl+Alt+U captures "
        + std::to_string(kCaptureFrames) + " frames).");
}

HRESULT WINAPI hkCreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    const HRESULT hr = oCreateDevice(adapter, fl, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&dev))) && dev) {
            HookDevice(dev);
            dev->Release();
        }
    }
    return hr;
}

const char* Fmt(int f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_R16_FLOAT: return "R16F";
    case DXGI_FORMAT_R8_UNORM: return "R8";
    case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
    default: return "fmt?";
    }
}

std::string Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

// Writes the capture: per frame, per submitted list, its passes (a new pass starts at each render-target change).
void Report() {
    std::vector<Submitted> subs;
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        subs.swap(g_submitted);
        g_open.clear();
        g_closed.clear();
    }
    std::unordered_map<const void*, PsoSig> pso;
    {
        std::shared_lock<std::shared_mutex> lock(g_psoMutex);
        pso = g_pso;
    }
    std::ostringstream o;
    o << "UI probe capture " << Stamp() << ": " << subs.size() << " command lists submitted over " << kCaptureFrames
      << " frames; " << pso.size() << " PSOs known.\n"
      << "Per list: passes (RT = first render target: size, format; VP = viewport; draws; PSOs used as #id), PIX events.\n"
      << "PSO #id: RTs/rt0 format, depth(test/write), blend src/dst, VS/PS bytecode sizes, PS hash, input elements.\n\n";
    std::map<int, const PsoSig*> usedPso;
    for (const Submitted& s : subs) {
        o << "== frame " << s.frame << "  queue " << s.queue << "  list " << s.list << "  (" << s.ev.size() << " events)\n";
        std::string rt = "(none)", vp = "(none)", events;
        int draws = 0;
        UINT idxMin = ~0u, idxMax = 0;
        std::vector<int> psos;
        auto flush = [&]() {
            if (draws == 0 && events.empty()) return;
            o << "   RT " << rt << "  VP " << vp << "  draws " << draws;
            if (draws) o << " (counts " << idxMin << ".." << idxMax << ")";
            if (!psos.empty()) { o << "  PSO"; for (int id : psos) o << " #" << id; }
            if (!events.empty()) o << "  events: " << events;
            o << "\n";
            draws = 0; idxMin = ~0u; idxMax = 0; psos.clear(); events.clear();
        };
        int curPso = 0;
        for (const Ev& e : s.ev) {
            switch (e.t) {
            case EvRT: {
                flush();
                char b[96];
                if (e.p) snprintf(b, sizeof(b), "%.0fx%.0f %s (%p)%s", e.a, e.b, Fmt(static_cast<int>(e.c)), e.p, e.m ? " +depth" : "");
                else snprintf(b, sizeof(b), "%u RTs, unknown%s", e.n, e.m ? " +depth" : "");
                rt = b;
                break;
            }
            case EvVP: {
                char b[64];
                snprintf(b, sizeof(b), "%.0f,%.0f %.0fx%.0f", e.a, e.b, e.c, e.d);
                vp = b;
                break;
            }
            case EvPSO: {
                auto it = pso.find(e.p);
                curPso = it != pso.end() ? it->second.id : -1;
                if (it != pso.end()) usedPso[curPso] = &it->second;
                break;
            }
            case EvDraw:
                ++draws;
                idxMin = (std::min)(idxMin, e.n);
                idxMax = (std::max)(idxMax, e.n);
                if (psos.empty() || psos.back() != curPso) { if (psos.size() < 12) psos.push_back(curPso); }
                break;
            case EvClear: {
                char b[64];
                snprintf(b, sizeof(b), "[clear %.0fx%.0f] ", e.a, e.b);
                events += b;
                break;
            }
            case EvBegin: events += "<" + e.name + "> "; break;
            case EvEnd: events += "</> "; break;
            default: break;
            }
        }
        flush();
    }
    o << "\nPSOs used:\n";
    for (const auto& kv : usedPso) {
        const PsoSig& s = *kv.second;
        char b[200];
        snprintf(b, sizeof(b), "  #%d: %u RT %s, dsv %d, depth %d/%d, blend %d (%d/%d), VS %zu PS %zu, PS hash %08X, inputs %u\n", s.id, s.numRT,
                 Fmt(s.rt0), static_cast<int>(s.dsv), s.depth ? 1 : 0, s.depthWrite ? 1 : 0, s.blend ? 1 : 0, static_cast<int>(s.src),
                 static_cast<int>(s.dst), s.vsSize, s.psSize, s.psHash, s.inputElems);
        o << b;
    }
    {
        std::unordered_map<ULONG, Stack> stacks;
        {
            std::lock_guard<std::mutex> lock(g_capMutex);
            stacks.swap(g_stacks);
        }
        o << "\nCall stacks of UI-pipeline binds (" << stacks.size() << " distinct; module+offset, innermost first):\n";
        for (const auto& kv : stacks) {
            const Stack& st = kv.second;
            o << "  x" << st.count << " (first in frame " << st.firstFrame << "):";
            for (USHORT i = 0; i < st.n; ++i) {
                HMODULE mod = nullptr;
                char path[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(st.f[i]), &mod) && mod) {
                    GetModuleFileNameA(mod, path, MAX_PATH);
                }
                const char* slash = strrchr(path, '\\');
                char b[96];
                snprintf(b, sizeof(b), " %s+0x%llX", slash ? slash + 1 : path,
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(st.f[i]) - reinterpret_cast<uintptr_t>(mod)));
                o << b;
            }
            o << "\n";
        }
    }
    const std::string name = "vrmod_uiprobe_" + Stamp() + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();
    modlog::Line("UIPROBE: capture written -> " + name + " (" + std::to_string(subs.size()) + " lists)");
    PlayCue(Cue::Success);
}

} // namespace

bool InstallUiProbe() {
    const HudSettings& hud = GetHudSettings();
    g_hudOn.store(hud.enabled);
    g_hudScale.store(hud.scale);
    g_hudOffY.store(hud.offsetY);
    if (!GetDebugSwitches().uiProbe && !hud.enabled) {
        modlog::Line("HUD: [hud] enabled=0 and no UI probe: D3D12 hooks not installed (Ctrl+Alt+7 needs a restart with enabled=1).");
        return false;
    }
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    void* target = d3d12 ? reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (!target || !EnsureMinHookInitialized()) {
        modlog::Line("UIPROBE: D3D12CreateDevice not found; probe off.");
        return false;
    }
    if (!Hook(target, reinterpret_cast<void*>(&hkCreateDevice), reinterpret_cast<void**>(&oCreateDevice), "D3D12CreateDevice")) return false;
    modlog::Line(std::string("UIPROBE/HUD: waiting for the game's D3D12 device (probe ") + (GetDebugSwitches().uiProbe ? "on" : "off")
        + ", HUD " + (hud.enabled ? "on" : "off") + ", scale " + std::to_string(hud.scale) + ").");
    return true;
}

#ifdef UI_PROBE_HARNESS
bool g_forceCapture = false;
void UiProbeForceCapture() { g_forceCapture = true; }
#endif

namespace {
void SaveHudLive() {
    HudSettings h;
    h.enabled = g_hudOn.load();
    h.scale = g_hudScale.load();
    h.offsetY = g_hudOffY.load();
    SetHudForSave(h);
}

void HudKeys() {
    static bool was[10] = {};
    const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    for (int d = 5; d <= 9; ++d) {
        const bool down = (VRMOD_DEV_KEY('0' + d) & 0x8000) != 0;
        const bool pressed = down && !was[d] && ca;
        was[d] = down;
        if (!pressed) continue;
        if (d == 7) {
            g_hudOn.store(!g_hudOn.load());
            PlayCue(g_hudOn.load() ? Cue::On : Cue::Off);
        } else if (d == 8 || d == 9) {
            float v = g_hudScale.load() + (d == 9 ? 0.05f : -0.05f);
            v = v < 0.2f ? 0.2f : (v > 1.0f ? 1.0f : v);
            g_hudScale.store(v);
        } else {
            float v = g_hudOffY.load() + (d == 6 ? 0.02f : -0.02f);
            v = v < -0.4f ? -0.4f : (v > 0.4f ? 0.4f : v);
            g_hudOffY.store(v);
        }
        SaveHudLive();
        char b[160];
        snprintf(b, sizeof(b), "HUD: %s, scale %.2f, offset_y %+.2f (Ctrl+Alt+0 saves)", g_hudOn.load() ? "on" : "OFF", g_hudScale.load(), g_hudOffY.load());
        modlog::Line(b);
    }
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 30000) {
        last = now;
        const uint32_t n = g_hudRewrites.exchange(0);
        if (g_deviceHooked.load()) {
            char b[160];
            snprintf(b, sizeof(b), "HUD (30 s): %u UI viewport rewrites; %s, scale %.2f, offset_y %+.2f", n, g_hudOn.load() ? "on" : "OFF",
                     g_hudScale.load(), g_hudOffY.load());
            modlog::Line(b);
        }
    }
}
}

bool UiProbeHudGet(bool* on, float* scale, float* offsetY) {
    *on = g_hudOn.load();
    *scale = g_hudScale.load();
    *offsetY = g_hudOffY.load();
    return g_deviceHooked.load();
}

void UiProbeHudSet(bool on, float scale, float offsetY) {
    g_hudOn.store(on);
    g_hudScale.store(scale < 0.2f ? 0.2f : (scale > 1.0f ? 1.0f : scale));
    g_hudOffY.store(offsetY < -0.4f ? -0.4f : (offsetY > 0.4f ? 0.4f : offsetY));
    SaveHudLive();
}

bool UiProbeSetExecuteFilter(UiProbeExecuteFilter filter) {
    if (!oExecute) return false;
    g_executeFilter.store(filter);
    return true;
}

void BodyCensusOnPresent() {
    static bool was = false;
    const bool down = (VRMOD_DEV_KEY(VK_F12) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool pressed = down && !was;
    was = down;
    const int f = g_bodyCensusFrame.load();
    if (f < 0) {
        if (!pressed) return;
        { std::lock_guard<std::mutex> lock(g_bodyCensusMutex); g_bodyCensus.clear(); }
        g_bodyCensusFrame.store(0);
        modlog::Line("BODYCENSUS: recording 3 frames of draws");
        return;
    }
    if (f < 2) { g_bodyCensusFrame.store(f + 1); return; }
    g_bodyCensusFrame.store(-1);
    std::vector<CensusDraw> all;
    { std::lock_guard<std::mutex> lock(g_bodyCensusMutex); all.swap(g_bodyCensus); }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[64];
    snprintf(name, sizeof(name), "vrmod_bodycensus_%04d%02d%02d_%02d%02d%02d.txt", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    FILE* out = fopen(name, "w");
    int faces = 0;
    if (out) {
        fprintf(out, "frame list ib count first table b0 b1 face\n");
        for (const CensusDraw& c : all) {
            faces += c.face;
            fprintf(out, "%d %p %llX %u %u %llX %llX %llX %d\n", c.frame, c.list, static_cast<unsigned long long>(c.ib), c.count, c.first,
                    static_cast<unsigned long long>(c.table), static_cast<unsigned long long>(c.b0), static_cast<unsigned long long>(c.b1), c.face ? 1 : 0);
        }
        fclose(out);
    }
    char b[200];
    snprintf(b, sizeof(b), "BODYCENSUS: %zu draws, %d face draws -> %s", all.size(), faces, name);
    modlog::Line(b);
    PlayCue(faces ? Cue::Success : Cue::Failure);
}

void UiProbeOnPresent() {
    BodyCensusOnPresent();
    FaceTrackBodyTick(g_groupIb[kHead].load(std::memory_order_relaxed));
    HudKeys();
    // Ctrl+Alt+U capture: available whenever the hooks are in (2026-10-09: with the [debug] ui_probe gate a capture
    // never started in a session that had it on -- every press is now logged with the state that decides it).
    static bool uWas = false;
    bool u = (VRMOD_DEV_KEY('U') & 0x8000) != 0;
    bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    if (u && !uWas) {   // diagnostic: does the key reach us, and with which modifiers?
        modlog::Line(std::string("UIPROBE: U pressed (Ctrl ") + ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? "down" : "up") + ", Alt "
            + ((GetAsyncKeyState(VK_MENU) & 0x8000) ? "down" : "up") + ")");
    }
    if (u && !uWas && ca && (!g_deviceHooked.load() || g_captureLeft.load() != 0)) {
        modlog::Line(std::string("UIPROBE: Ctrl+Alt+U ignored -- ") + (!g_deviceHooked.load() ? "the game's D3D12 device isn't hooked" : "a capture is still running"));
        PlayCue(Cue::Failure);
    }
#ifdef UI_PROBE_HARNESS
    if (g_forceCapture) { g_forceCapture = false; u = true; ca = true; uWas = false; }
#endif
    if (u && !uWas && ca && g_deviceHooked.load() && g_captureLeft.load() == 0) {
        {
            std::lock_guard<std::mutex> lock(g_capMutex);
            g_open.clear();
            g_closed.clear();
            g_submitted.clear();
            g_stacks.clear();
        }
        g_captureFrame = 0;
        g_captureLeft.store(kCaptureFrames + 1);   // the first Present starts frame 0
        modlog::Line("UIPROBE: capturing the next " + std::to_string(kCaptureFrames) + " frames.");
        PlayCue(Cue::On);
    }
    uWas = u;
    const int left = g_captureLeft.load();
    if (left > 0) {
        if (left == kCaptureFrames + 1) {
            g_capturing.store(true);
        } else {
            ++g_captureFrame;
        }
        if (g_captureLeft.fetch_sub(1) == 1) {
            g_capturing.store(false);
            Report();
        }
    }
}

void UiProbeSetAimOffset(float ndcX, float ndcY) {
    g_hudAimX.store(ndcX, std::memory_order_relaxed);
    g_hudAimY.store(ndcY, std::memory_order_relaxed);
}

namespace {
bool WorldIconDrawI(ID3D12GraphicsCommandList* cl, UINT v, UINT inst, UINT sv, UINT si) {
    return WorldIconDraw(cl, [&] { oDrawI(cl, v, inst, sv, si); });
}
bool WorldIconDrawII(ID3D12GraphicsCommandList* cl, UINT idx, UINT inst, UINT si, INT bv, UINT sInst) {
    return WorldIconDraw(cl, [&] { oDrawII(cl, idx, inst, si, bv, sInst); });
}
}  // namespace

void UiProbeMarkNotUi(const void* pso) {
    std::unique_lock<std::shared_mutex> lock(g_psoMutex);
    auto it = g_pso.find(pso);
    if (it != g_pso.end()) it->second.ui = false;
}

void UiProbeSetHideGroups(bool head, bool bow, bool quiver, bool body) {
    g_groupOn[kHead].store(head, std::memory_order_relaxed);
    g_groupOn[kBow].store(bow, std::memory_order_relaxed);
    g_groupOn[kQuiver].store(quiver, std::memory_order_relaxed);
    g_groupOn[kBody].store(body, std::memory_order_relaxed);
    g_groupOn[kHair].store(head, std::memory_order_relaxed);
}

std::string UiProbeGearTestStep() {
    if (g_gearTestIndex == 0 && !g_censusDone.load()) {
        g_censusWant.store(true);
        return "gear finder: listing the models drawn around Lara's head -- press again in a moment";
    }
    std::lock_guard<std::mutex> lock(g_censusMutex);
    if (g_gearTestIndex == 0) {
        std::string list = "gear finder: " + std::to_string(g_candidates.size()) + " candidate models near the head draw:";
        for (size_t i = 0; i < g_candidates.size(); ++i) {
            list += " [" + std::to_string(i + 1) + "]";
            for (const Part& q : g_candidates[i].parts) list += " " + std::to_string(q.count) + "@" + std::to_string(q.first);
        }
        modlog::Line(list);
    }
    g_gearTestIndex = g_gearTestIndex >= static_cast<int>(g_candidates.size()) ? 0 : g_gearTestIndex + 1;
    if (g_gearTestIndex == 0) {
        g_gearTestIb.store(0);
        g_censusDone.store(false);   // the next press lists afresh
        return "gear finder off (nothing extra hidden)";
    }
    const GearCandidate& c = g_candidates[g_gearTestIndex - 1];
    g_gearTestIb.store(c.ibVa);
    std::string out = "gear finder: candidate " + std::to_string(g_gearTestIndex) + " of " + std::to_string(g_candidates.size()) + " hidden --";
    for (const Part& q : c.parts) out += " " + std::to_string(q.count) + "@" + std::to_string(q.first);
    return out;
}

int UiProbeGearTestIndex() { return g_gearTestIndex; }

std::string UiProbeHideStats() {
    std::string out;
    for (int g = 0; g < kGroups; ++g) {
        char b[96];
        const uint64_t learned = g_groupLearned[g].exchange(0, std::memory_order_relaxed);
        snprintf(b, sizeof(b), "%s%s %llu skipped%s", g ? ", " : "", kGroupName[g], static_cast<unsigned long long>(g_groupSkipped[g].exchange(0)),
                 g_groupIb[g].load() == 0 ? " (not seen)" : learned ? " (buffer learned)" : "");
        out += b;
    }
    char h[192];
    const uint64_t kept = g_hairDepthKept.exchange(0);
    snprintf(h, sizeof(h), " (strand draw %u indices), hair composite %llu, hair depth %llu skipped; shadow strands kept: %u indices, DSV format %u",
             g_hairCount.load(), static_cast<unsigned long long>(g_hairCompositeSkipped.exchange(0)),
             static_cast<unsigned long long>(g_hairDepthSkipped.exchange(0)), static_cast<UINT>(kept), static_cast<UINT>(kept >> 32));
    out += h;
    if (const uint64_t r = g_worldRestores.exchange(0)) out += "; world-icon passes with other draws after the icons: " + std::to_string(r);
    return out;
}

}
