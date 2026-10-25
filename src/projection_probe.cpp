#include "projection_probe.h"

#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "aer.h"
#include "audio_cue.h"
#include "camera_interceptor.h"
#include "logger.h"
#include "openxr_integration.h"
#include "settings.h"

namespace vrmod {

namespace {

constexpr float kDelta = 0.034906585f;   // 2 degrees
constexpr int kSettle = 20, kCaptures = 3;
constexpr UINT kStripLong = 2048, kStripShort = 32;

struct Step { float yaw, pitch; };
const Step kSteps[4] = { { kDelta, 0 }, { -kDelta, 0 }, { 0, kDelta }, { 0, -kDelta } };

struct State {
    bool running = false;
    int step = 0, count = 0, captured = 0;
    bool heldBefore = false;
    std::vector<double> prof[4];   // steps 0/1: column profile of the horizontal strip; 2/3: row profile of the vertical strip
    UINT w = 0, h = 0, hw = 0, vh = 0;
    ID3D12Resource* readback = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE evt = nullptr;
    UINT64 fenceValue = 0;
    UINT hPitch = 0;
    UINT64 vOffset = 0, totalBytes = 0;
    bool bgra = false;
};
State g;

void Release() {
    if (g.fence) { g.fence->Release(); g.fence = nullptr; }
    if (g.list) { g.list->Release(); g.list = nullptr; }
    if (g.alloc) { g.alloc->Release(); g.alloc = nullptr; }
    if (g.readback) { g.readback->Release(); g.readback = nullptr; }
    if (g.evt) { CloseHandle(g.evt); g.evt = nullptr; }
}

bool Setup(ID3D12Device* device, ID3D12Resource* bb) {
    const D3D12_RESOURCE_DESC d = bb->GetDesc();
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    g.bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!rgba && !g.bgra) return false;
    g.w = static_cast<UINT>(d.Width);
    g.h = d.Height;
    g.hw = (std::min)(kStripLong, g.w - 64);
    g.vh = (std::min)(kStripLong, g.h - 64);
    g.hPitch = (g.hw * 4 + 255) & ~255u;
    g.vOffset = (static_cast<UINT64>(g.hPitch) * kStripShort + 511) & ~511ull;
    g.totalBytes = g.vOffset + 256ull * g.vh;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = g.totalBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool ok = g.evt
        && SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.readback)))
        && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc)))
        && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.list)))
        && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
    if (!ok) { Release(); return false; }
    g.list->Close();
    return true;
}

// Reads both centre strips of this Present's backbuffer and adds the relevant profile for the current step.
bool Capture(ID3D12CommandQueue* queue, ID3D12Resource* bb) {
    const D3D12_RESOURCE_DESC d = bb->GetDesc();
    if (d.Width != g.w || d.Height != g.h) return false;
    g.alloc->Reset();
    g.list->Reset(g.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint = { d.Format, g.hw, kStripShort, 1, g.hPitch };
        const UINT x0 = g.w / 2 - g.hw / 2, y0 = g.h / 2 - kStripShort / 2;
        D3D12_BOX box{ x0, y0, 0, x0 + g.hw, y0 + kStripShort, 1 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = g.vOffset;
        dst.PlacedFootprint.Footprint = { d.Format, kStripShort, g.vh, 1, 256 };
        const UINT x0 = g.w / 2 - kStripShort / 2, y0 = g.h / 2 - g.vh / 2;
        D3D12_BOX box{ x0, y0, 0, x0 + kStripShort, y0 + g.vh, 1 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g.list->ResourceBarrier(1, &b);
    g.list->Close();
    ID3D12CommandList* lists[] = { g.list };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g.fence, ++g.fenceValue);
    g.fence->SetEventOnCompletion(g.fenceValue, g.evt);
    if (WaitForSingleObject(g.evt, 3000) != WAIT_OBJECT_0) return false;
    void* mapped = nullptr;
    D3D12_RANGE range{ 0, static_cast<SIZE_T>(g.totalBytes) };
    if (FAILED(g.readback->Map(0, &range, &mapped)) || !mapped) return false;
    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    const int ri = g.bgra ? 2 : 0, bi = g.bgra ? 0 : 2;
    auto luma = [&](const uint8_t* p) { return 0.2126 * p[ri] + 0.7152 * p[1] + 0.0722 * p[bi]; };
    std::vector<double>& prof = g.prof[g.step];
    if (g.step < 2) {   // yaw: column profile of the horizontal strip
        prof.resize(g.hw, 0.0);
        for (UINT y = 0; y < kStripShort; ++y)
            for (UINT x = 0; x < g.hw; ++x) prof[x] += luma(base + static_cast<size_t>(y) * g.hPitch + x * 4);
    } else {            // pitch: row profile of the vertical strip
        prof.resize(g.vh, 0.0);
        for (UINT y = 0; y < g.vh; ++y)
            for (UINT x = 0; x < kStripShort; ++x) prof[y] += luma(base + g.vOffset + static_cast<size_t>(y) * 256 + x * 4);
    }
    D3D12_RANGE none{ 0, 0 };
    g.readback->Unmap(0, &none);
    return true;
}

// Shift s (pixels, sub-pixel) such that b[x + s] best matches a[x]; mean-removed profiles, SAD, parabola refinement.
double Shift(std::vector<double> a, std::vector<double> b, double* quality) {
    const int n = static_cast<int>((std::min)(a.size(), b.size()));
    auto demean = [n](std::vector<double>& v) { double m = 0; for (int i = 0; i < n; ++i) m += v[i]; m /= n; for (int i = 0; i < n; ++i) v[i] -= m; };
    demean(a);
    demean(b);
    const int maxS = (std::min)(600, n / 3);
    std::vector<double> cost(2 * maxS + 1);
    for (int s = -maxS; s <= maxS; ++s) {
        double sum = 0;
        int cnt = 0;
        for (int x = maxS; x < n - maxS; ++x) { sum += std::fabs(a[x] - b[x + s]); ++cnt; }
        cost[s + maxS] = sum / (std::max)(1, cnt);
    }
    int best = -maxS;
    for (int s = -maxS; s <= maxS; ++s) if (cost[s + maxS] < cost[best + maxS]) best = s;
    double s = best;
    if (best > -maxS && best < maxS) {
        const double c0 = cost[best - 1 + maxS], c1 = cost[best + maxS], c2 = cost[best + 1 + maxS];
        const double den = c0 - 2 * c1 + c2;
        if (den > 1e-9) s += 0.5 * (c0 - c2) / den;
    }
    // Quality: how much deeper the best match is than the average cost (1 = no structure).
    double mean = 0;
    for (double c : cost) mean += c;
    mean /= cost.size();
    *quality = cost[best + maxS] > 0 ? mean / cost[best + maxS] : 0.0;
    return s;
}

// Exact pinhole fit. For a pure rotation by 2 delta between the two images, a pixel at offset u from the image centre
// in image a shows what image b has at u' = f * tan(atan(u / f) - 2 delta) (the same form for yaw along the centre row
// and pitch along the centre column). A plain shift assumes u' = u - 2 f tan(delta) everywhere, but off-centre
// features move further (sec^2), which biased the first in-game runs by ~1 %. Searches f around the shift-based
// estimate f0 and returns the f with the lowest mean |a - b(u')| (mean-removed, linear interpolation).
double FitFocal(std::vector<double> a, std::vector<double> b, double f0, double* costOut) {
    const int n = static_cast<int>((std::min)(a.size(), b.size()));
    auto demean = [n](std::vector<double>& v) { double m = 0; for (int i = 0; i < n; ++i) m += v[i]; m /= n; for (int i = 0; i < n; ++i) v[i] -= m; };
    demean(a);
    demean(b);
    const double half = 0.5 * n, t2 = 2.0 * kDelta;
    auto cost = [&](double f) {
        double sum = 0;
        int cnt = 0;
        for (int i = 0; i < n; ++i) {
            const double u = i + 0.5 - half;
            const double up = f * std::tan(std::atan(u / f) - t2);
            const double j = up + half - 0.5;
            const int j0 = static_cast<int>(std::floor(j));
            if (j0 < 0 || j0 + 1 >= n) continue;
            const double w = j - j0;
            sum += std::fabs(a[i] - (b[j0] * (1.0 - w) + b[j0 + 1] * w));
            ++cnt;
        }
        return cnt > n / 3 ? sum / cnt : 1e300;
    };
    double best = f0, bestC = cost(f0);
    for (double k = 0.85; k <= 1.15; k += 0.001) { const double c = cost(f0 * k); if (c < bestC) { bestC = c; best = f0 * k; } }
    const double coarse = best;
    for (double k = -0.0015; k <= 0.0015; k += 0.00005) { const double c = cost(coarse * (1.0 + k)); if (c < bestC) { bestC = c; best = coarse * (1.0 + k); } }
    *costOut = bestC;
    return best;
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void Report() {
    double qH = 0, qV = 0;
    const double sH = Shift(g.prof[0], g.prof[1], &qH);   // +yaw (left) image vs -yaw: content moves right for +yaw
    const double sV = Shift(g.prof[2], g.prof[3], &qV);
    const double t2 = 2.0 * std::tan(kDelta);
    // First estimate from the plain shift (biased, see FitFocal), then the exact pinhole fit.
    const double fxShift = std::fabs(sH) / t2, fyShift = std::fabs(sV) / t2;
    double cH = 0, cV = 0;
    const double fx = fxShift > 0 ? FitFocal(g.prof[0], g.prof[1], fxShift, &cH) : 0.0;
    const double fy = fyShift > 0 ? FitFocal(g.prof[2], g.prof[3], fyShift, &cV) : 0.0;
    const double tanH = fx > 0 ? (g.w * 0.5) / fx : 0.0, tanV = fy > 0 ? (g.h * 0.5) / fy : 0.0;
    const double tanHShift = fxShift > 0 ? (g.w * 0.5) / fxShift : 0.0, tanVShift = fyShift > 0 ? (g.h * 0.5) / fyShift : 0.0;
    const float fov = OpenXRAerGameFov();
    auto deg = [](double r) { return r * 57.29577951; };
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(4);
    o << "Projection probe " << TimeStamp() << "\n"
      << "backbuffer " << g.w << "x" << g.h << " (aspect " << static_cast<double>(g.w) / g.h << "); game FOV value written " << fov << " rad ("
      << deg(fov) << " deg); FOV model in use " << OpenXRFovModel() << "; strips " << g.hw << "x" << kStripShort << " / " << kStripShort << "x" << g.vh << "\n"
      << "yaw +-2 deg:   pinhole fit f_x " << fx << " px -> tanH " << tanH << " (half-angle " << deg(std::atan(tanH)) << " deg)   [plain shift "
      << sH << " px, quality " << qH << ", would give tanH " << tanHShift << "]\n"
      << "pitch +-2 deg: pinhole fit f_y " << fy << " px -> tanV " << tanV << " (half-angle " << deg(std::atan(tanV)) << " deg)   [plain shift "
      << sV << " px, quality " << qV << ", would give tanV " << tanVShift << "]\n"
      << "measured aspect of the frustum tanH/tanV " << (tanV > 0 ? tanH / tanV : 0.0) << "; pixel aspect f_x/f_y " << (fy > 0 ? fx / fy : 0.0)
      << " (1 = square pixels)\n\npredictions for the FOV value written (tanH x tanV, error vs measured):\n";
    int bestModel = -1;
    double bestErr = 1e9;
    for (int m = 0; m < 4; ++m) {
        float ph = 0, pv = 0;
        OpenXRPredictFrustum(fov, m, &ph, &pv);
        const double eh = tanH > 0 ? (ph / tanH - 1.0) * 100.0 : 0.0, ev = tanV > 0 ? (pv / tanV - 1.0) * 100.0 : 0.0;
        const double err = (std::max)(std::fabs(eh), std::fabs(ev));
        if (err < bestErr - 1e-6 || (std::fabs(err - bestErr) <= 1e-6 && m == OpenXRFovModel())) { bestErr = err; bestModel = m; }
        o << "  model " << m << (m == OpenXRFovModel() ? " (in use, = declared)" : "") << ": " << ph << " x " << pv << "   error H "
          << std::setprecision(2) << eh << " %, V " << ev << " %" << std::setprecision(4) << "\n";
    }
    o << "  plain vertical FOV (tanV = tan(fov/2), tanH = tanV x W/H): " << std::tan(fov * 0.5) * g.w / g.h << " x " << std::tan(fov * 0.5) << "\n\n"
      << "best matching model " << bestModel << " (worst-axis error " << std::setprecision(2) << bestErr << " %)"
      << (bestModel == OpenXRFovModel() ? " = the model in use: the declared frustum matches what the game renders" : " -- NOT the model in use")
      << "\npass: best model = the model in use (models with identical predictions count as the same) and error <= 1.5 %\n";
    const std::string name = "vrmod_projection_" + TimeStamp() + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();
    std::ostringstream l;
    l.setf(std::ios::fixed);
    l.precision(4);
    l << "PROJECTION: measured tanH " << tanH << " tanV " << tanV << "; best model " << bestModel << " (error " << std::setprecision(2) << bestErr
      << " %), in use " << OpenXRFovModel() << " -> " << name;
    modlog::Line(l.str());
    PlayCue(bestModel == OpenXRFovModel() && bestErr < 1.5 ? Cue::Success : Cue::Failure);
}

void Finish() {
    AerProbeSet(false, 0, 0);
    AerSetHoldBase(g.heldBefore);
    Release();
    g.running = false;
}

bool CtrlOPressed() {
    static bool was = false;
    const bool d = (GetAsyncKeyState('O') & 0x8000) != 0;
    const bool p = d && !was;
    was = d;
    return p && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
}

} // namespace

void ProjectionProbeOnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) {
    if (!device || !queue || !backbuffer) return;
    const bool pressed = CtrlOPressed();
    if (!g.running) {
        if (!pressed) return;
        if (GetRenderMode() != RenderMode::Aer || MenuScreenActive() || !IsHeadTrackingEnabled() || OpenXRAerGameFov() <= 0.0f) {
            modlog::Line("PROJECTION: needs [render] mode=aer, gameplay and head tracking on.");
            PlayCue(Cue::Failure);
            return;
        }
        if (!Setup(device, backbuffer)) {
            modlog::Line("PROJECTION: could not set up the readback (backbuffer format?).");
            PlayCue(Cue::Failure);
            return;
        }
        for (auto& p : g.prof) p.clear();
        g.heldBefore = AerHoldBase();
        AerSetHoldBase(true);
        g.running = true;
        g.step = 0;
        g.count = 0;
        g.captured = 0;
        AerProbeSet(true, kSteps[0].yaw, kSteps[0].pitch);
        modlog::Line("PROJECTION: probe started (~3 s; camera held, head frozen, left eye only).");
        PlayCue(Cue::On);
        return;
    }
    if (pressed || MenuScreenActive()) {
        modlog::Line("PROJECTION: cancelled.");
        Finish();
        PlayCue(Cue::Failure);
        return;
    }
    if (++g.count <= kSettle) return;
    if (!Capture(queue, backbuffer)) {
        modlog::Line("PROJECTION: readback failed or the resolution changed.");
        Finish();
        PlayCue(Cue::Failure);
        return;
    }
    if (++g.captured < kCaptures) return;
    g.captured = 0;
    g.count = 0;
    if (++g.step < 4) {
        AerProbeSet(true, kSteps[g.step].yaw, kSteps[g.step].pitch);
        return;
    }
    Finish();
    Report();
}

} // namespace vrmod
