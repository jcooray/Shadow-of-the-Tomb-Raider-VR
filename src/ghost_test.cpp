#include "ghost_test.h"

#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "audio_cue.h"
#include "bmp_writer.h"
#include "camera_interceptor.h"
#include "frame_log.h"
#include "head_tracking.h"
#include "logger.h"
#include "perf_stats.h"
#include "settings.h"

namespace vrmod {

namespace {

constexpr uint32_t kThumbTargetWidth = 512;
constexpr int kSettlePresents = 30;   // > render latency (~3) plus any temporal accumulation settling
constexpr int kSheetColumns = 6;
constexpr int kTile = 32;             // thumbnail pixels per tile side, for the local ghost measure

// Only "still" tiles are measured: where the two poses look different, and where each pose's early and late held
// frames agree (so nothing in that tile moved during the test -- the first in-game runs were spoiled by Lara's
// hair). Limits (see docs/PHASE3.md):
constexpr double kMinSeparation = 10.0;     // tile rms luma difference between the two poses (0..255)
constexpr double kMaxStillChange = 3.0;     // tile rms, early vs late held frames of the same pose
constexpr int kMinStillTiles = 20;          // fewer = INVALID (scene moved too much)
constexpr double kMaxGhost = 0.02;          // opposite-pose weight over the still tiles
constexpr double kTileGhost = 0.08;         // a still tile with more than this is "ghosted"...
constexpr double kMaxGhostedTiles = 0.05;   // ...and at most this fraction of the measured still tiles may be
constexpr double kMinTileStructure = 5.0;   // tile rms of the other pose's detail (mean removed) needed to measure it
constexpr double kMaxResidual = 3.0;        // rms luma left after the fit (smearing, e.g. motion blur, stays here)

struct Step {
    FlipMode mode;
    int captures;
    char tag;   // P / N = held +60 / -60 (early), A = alternating, Q / R = held +60 / -60 (late)
};
const Step kSteps[] = {
    { FlipMode::HoldPlus, 3, 'P' },
    { FlipMode::HoldMinus, 3, 'N' },
    { FlipMode::Alternate, 12, 'A' },
    { FlipMode::HoldPlus, 3, 'Q' },
    { FlipMode::HoldMinus, 3, 'R' },
};
constexpr int kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

struct Capture {
    char tag = '?';
    uint64_t presentFrame = 0;
    double tMs = 0.0;
    bool havePose = false;
    CamPose pose{};               // the camera struct at capture time (position cm, rotation incl. the flip)
    std::vector<float> luma;      // tw * th, 0..255
    std::vector<uint8_t> rgba;    // tw * th * 4, for the contact sheet
};

struct State {
    bool running = false;
    int step = 0;
    int settle = 0;
    int captured = 0;
    uint32_t tw = 0, th = 0, factor = 1;
    std::vector<Capture> caps;
    std::string settingsReport;
    ID3D12Resource* readback = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE evt = nullptr;
    UINT64 fenceValue = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes = 0;
    bool bgra = false;
    int blackoutTail = 0;   // Presents to keep the headset black after the test (flipped frames still in flight)
};
State g;
constexpr int kBlackoutTailPresents = 15;

void ReleaseResources() {
    if (g.fence) { g.fence->Release(); g.fence = nullptr; }
    if (g.list) { g.list->Release(); g.list = nullptr; }
    if (g.alloc) { g.alloc->Release(); g.alloc = nullptr; }
    if (g.readback) { g.readback->Release(); g.readback = nullptr; }
    if (g.evt) { CloseHandle(g.evt); g.evt = nullptr; }
}

void Finish() {
    FrameLogForceFlipMode(FlipMode::Off);
    HeadTrackingFreeze(false);
    CameraReasonEnd(ThirdPersonReason::Diagnostic);
    ReleaseResources();
    g.running = false;
    g.blackoutTail = kBlackoutTailPresents;
}

void Abort(const std::string& why) {
    modlog::Line("GHOST: test aborted -- " + why);
    Finish();
    g.caps.clear();
    PlayCue(Cue::Failure);
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Value of "key:" in the game's settings block (the token after the colon), or "" if absent.
std::string SettingValue(const std::string& block, const std::string& key) {
    size_t pos = 0;
    while ((pos = block.find(key + ":", pos)) != std::string::npos) {
        const bool startOk = pos == 0 || block[pos - 1] == ' ' || block[pos - 1] == '\t' || block[pos - 1] == ','
            || block[pos - 1] == '\n';
        pos += key.size() + 1;
        if (!startOk) continue;
        while (pos < block.size() && (block[pos] == ' ' || block[pos] == '\t')) ++pos;
        size_t end = pos;
        while (end < block.size() && block[end] != ' ' && block[end] != ',' && block[end] != '\n' && block[end] != '\r') ++end;
        return block.substr(pos, end - pos);
    }
    return "";
}

bool Setup(ID3D12Device* device, ID3D12Resource* backbuffer) {
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    g.bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!rgba && !g.bgra) {
        modlog::Line("GHOST: backbuffer format " + std::to_string(desc.Format) + " is not 8-bit RGBA/BGRA; can't measure.");
        return false;
    }
    const uint32_t w = static_cast<uint32_t>(desc.Width), h = desc.Height;
    g.factor = (std::max)(1u, w / kThumbTargetWidth);
    g.tw = w / g.factor;
    g.th = h / g.factor;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &g.footprint, nullptr, nullptr, &g.totalBytes);

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
    if (!ok) {
        modlog::Line("GHOST: could not create readback resources.");
        ReleaseResources();
        return false;
    }
    g.list->Close();
    g.fenceValue = 0;
    return true;
}

// Copies the backbuffer (in PRESENT state, frame finished) to the CPU and reduces it to a thumbnail.
bool CaptureFrame(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, Capture* out) {
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    if (desc.Width / g.factor != g.tw || desc.Height / g.factor != g.th) {
        return false;   // resolution changed mid-test
    }
    g.alloc->Reset();
    g.list->Reset(g.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = backbuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = g.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = backbuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g.list->ResourceBarrier(1, &b);
    g.list->Close();
    ID3D12CommandList* lists[] = { g.list };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g.fence, ++g.fenceValue);
    g.fence->SetEventOnCompletion(g.fenceValue, g.evt);
    if (WaitForSingleObject(g.evt, 5000) != WAIT_OBJECT_0) {
        return false;
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(g.totalBytes) };
    if (FAILED(g.readback->Map(0, &readRange, &mapped)) || !mapped) {
        return false;
    }
    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    const uint32_t pitch = g.footprint.Footprint.RowPitch;
    const int ri = g.bgra ? 2 : 0, bi = g.bgra ? 0 : 2;
    out->luma.assign(static_cast<size_t>(g.tw) * g.th, 0.0f);
    out->rgba.assign(static_cast<size_t>(g.tw) * g.th * 4, 255);
    std::vector<uint32_t> acc(static_cast<size_t>(g.tw) * 3);
    const double inv = 1.0 / (static_cast<double>(g.factor) * g.factor);
    for (uint32_t ty = 0; ty < g.th; ++ty) {
        std::fill(acc.begin(), acc.end(), 0u);
        for (uint32_t sy = 0; sy < g.factor; ++sy) {
            const uint8_t* row = base + static_cast<size_t>(ty * g.factor + sy) * pitch;
            for (uint32_t tx = 0; tx < g.tw; ++tx) {
                const uint8_t* px = row + static_cast<size_t>(tx) * g.factor * 4;
                uint32_t r = 0, gg = 0, bb = 0;
                for (uint32_t sx = 0; sx < g.factor; ++sx, px += 4) {
                    r += px[ri];
                    gg += px[1];
                    bb += px[bi];
                }
                acc[tx * 3] += r;
                acc[tx * 3 + 1] += gg;
                acc[tx * 3 + 2] += bb;
            }
        }
        for (uint32_t tx = 0; tx < g.tw; ++tx) {
            const double r = acc[tx * 3] * inv, gg = acc[tx * 3 + 1] * inv, bb = acc[tx * 3 + 2] * inv;
            const size_t i = static_cast<size_t>(ty) * g.tw + tx;
            out->luma[i] = static_cast<float>(0.2126 * r + 0.7152 * gg + 0.0722 * bb);
            out->rgba[i * 4] = static_cast<uint8_t>(r + 0.5);
            out->rgba[i * 4 + 1] = static_cast<uint8_t>(gg + 0.5);
            out->rgba[i * 4 + 2] = static_cast<uint8_t>(bb + 0.5);
        }
    }
    D3D12_RANGE none{ 0, 0 };
    g.readback->Unmap(0, &none);
    return true;
}

// ---- analysis ----

std::vector<float> Mean(const std::vector<const Capture*>& cs) {
    std::vector<float> m(cs[0]->luma.size(), 0.0f);
    for (const Capture* c : cs) for (size_t i = 0; i < m.size(); ++i) m[i] += c->luma[i];
    for (float& v : m) v /= static_cast<float>(cs.size());
    return m;
}

double Rms(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) { const double d = a[i] - b[i]; s += d * d; }
    return std::sqrt(s / (std::max)(size_t(1), a.size()));
}

// Solves the n x n system m x = b in place (Gaussian elimination, partial pivoting). False if singular.
bool SolveN(int n, std::vector<double>& m, std::vector<double>& b, std::vector<double>& x) {
    auto at = [&](int r, int c) -> double& { return m[static_cast<size_t>(r) * n + c]; };
    for (int c = 0; c < n; ++c) {
        int p = c;
        for (int r = c + 1; r < n; ++r) if (std::fabs(at(r, c)) > std::fabs(at(p, c))) p = r;
        if (std::fabs(at(p, c)) < 1e-12) return false;
        if (p != c) { for (int j = 0; j < n; ++j) std::swap(at(p, j), at(c, j)); std::swap(b[p], b[c]); }
        for (int r = c + 1; r < n; ++r) {
            const double f = at(r, c) / at(c, c);
            for (int j = c; j < n; ++j) at(r, j) -= f * at(c, j);
            b[r] -= f * b[c];
        }
    }
    x.assign(n, 0.0);
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int j = r + 1; j < n; ++j) s -= at(r, j) * x[j];
        x[r] = s / at(r, r);
    }
    return true;
}

// The game's auto-exposure keeps adapting during the test (it follows the average brightness, which changes between
// holding one pose and alternating), and its tone mapping makes that change non-linear in the 8-bit picture. So a
// frame is compared with a reference through a tone curve fitted by least squares: piecewise linear over
// kToneKnots evenly spaced knots on 0..255 (so it can follow any smooth brightness mapping -- a 3-parameter curve
// could not, and the leftover leaked into the ghost term), with a light smoothness penalty so knots with few pixels
// follow their neighbours. Optionally plus ghost * d, the other pose's difference.
constexpr int kToneKnots = 16;

struct ToneFit {
    bool ok = false;
    double knot[kToneKnots] = {};
    double ghost = 0.0;
    static void Weights(double ref, int* j, double* w1) {
        double u = ref / 255.0 * (kToneKnots - 1);
        if (u < 0.0) u = 0.0;
        if (u > kToneKnots - 1) u = kToneKnots - 1;
        int k = static_cast<int>(u);
        if (k > kToneKnots - 2) k = kToneKnots - 2;
        *j = k;
        *w1 = u - k;
    }
    double Tone(double ref) const {
        int j; double w1;
        Weights(ref, &j, &w1);
        return knot[j] * (1.0 - w1) + knot[j + 1] * w1;
    }
};

ToneFit FitTone(const std::vector<float>& y, const std::vector<float>& ref, const std::vector<float>* d,
                const std::vector<size_t>& px) {
    ToneFit f;
    if (px.empty()) return f;
    const int n = kToneKnots + (d ? 1 : 0);
    std::vector<double> m(static_cast<size_t>(n) * n, 0.0), b(n, 0.0), x;
    auto at = [&](int r, int c) -> double& { return m[static_cast<size_t>(r) * n + c]; };
    const int gi = kToneKnots;   // index of the ghost unknown
    for (size_t i : px) {
        int j; double w1;
        ToneFit::Weights(ref[i], &j, &w1);
        const double w0 = 1.0 - w1, yi = y[i];
        at(j, j) += w0 * w0; at(j, j + 1) += w0 * w1; at(j + 1, j) += w0 * w1; at(j + 1, j + 1) += w1 * w1;
        b[j] += w0 * yi; b[j + 1] += w1 * yi;
        if (d) {
            const double di = (*d)[i];
            at(j, gi) += w0 * di; at(gi, j) += w0 * di;
            at(j + 1, gi) += w1 * di; at(gi, j + 1) += w1 * di;
            at(gi, gi) += di * di;
            b[gi] += di * yi;
        }
    }
    // Smoothness: lambda * sum (knot[k+1] - knot[k])^2, scaled to the pixel count; keeps sparse knots well defined.
    const double lambda = 1e-4 * static_cast<double>(px.size());
    for (int k = 0; k + 1 < kToneKnots; ++k) {
        at(k, k) += lambda; at(k + 1, k + 1) += lambda; at(k, k + 1) -= lambda; at(k + 1, k) -= lambda;
    }
    f.ok = SolveN(n, m, b, x);
    if (f.ok) {
        for (int k = 0; k < kToneKnots; ++k) f.knot[k] = x[k];
        f.ghost = d ? x[gi] : 0.0;
    }
    return f;
}

double FitResidual(const ToneFit& f, const std::vector<float>& y, const std::vector<float>& ref,
                   const std::vector<float>* d, const std::vector<size_t>& px) {
    double s = 0.0;
    for (size_t i : px) {
        const double e = y[i] - f.Tone(ref[i]) - (d ? f.ghost * (*d)[i] : 0.0);
        s += e * e;
    }
    return px.empty() ? 0.0 : std::sqrt(s / px.size());
}

struct FrameResult {
    char cls = '?';
    double ghost = std::nan(""), exposure = std::nan(""), resid = std::nan("");
    double ghostedFrac = std::nan(""), tileGhostMax = std::nan("");
    std::vector<std::pair<size_t, double>> ghostedTiles;   // (tile index, local ghost) above kTileGhost
    bool clean = false;
};

// Each alternating frame A is compared with the held frames of its own pose (own) and of the other (other), on
// still tiles only:   A = tone(own) + ghost * (other - own).   tone() absorbs the exposure change; ghost is the
// fraction of the other pose mixed in; what the fit can't explain (e.g. a smear) is left in the residual.
void Analyse() {
    std::vector<const Capture*> P, N, A, Q, R;
    for (const Capture& c : g.caps) {
        if (c.tag == 'P') P.push_back(&c);
        else if (c.tag == 'N') N.push_back(&c);
        else if (c.tag == 'A') A.push_back(&c);
        else if (c.tag == 'Q') Q.push_back(&c);
        else if (c.tag == 'R') R.push_back(&c);
    }
    const size_t n = g.caps[0].luma.size();
    const std::vector<float> ep = Mean(P), lp = Mean(Q), en = Mean(N), ln = Mean(R);

    const uint32_t nx = g.tw / kTile, ny = g.th / kTile;
    auto tilePixels = [&](uint32_t tx, uint32_t ty, std::vector<size_t>* out) {
        for (uint32_t y = ty * kTile; y < (ty + 1) * kTile; ++y)
            for (uint32_t x = tx * kTile; x < (tx + 1) * kTile; ++x) out->push_back(static_cast<size_t>(y) * g.tw + x);
    };
    std::vector<std::vector<size_t>> tilePx(static_cast<size_t>(nx) * ny);
    std::vector<size_t> allPx;
    for (uint32_t ty = 0; ty < ny; ++ty) for (uint32_t tx = 0; tx < nx; ++tx) {
        tilePixels(tx, ty, &tilePx[ty * nx + tx]);
        tilePixels(tx, ty, &allPx);
    }

    // Change between a pose's early and late held frames, per tile, after matching exposure: fit early = tone(late)
    // over all tiles, then again over the better-fitting half (so moving things don't bend the curve).
    auto tileChange = [&](const std::vector<float>& early, const std::vector<float>& late, ToneFit* fitOut) {
        ToneFit f = FitTone(early, late, nullptr, allPx);
        std::vector<double> change(tilePx.size(), 1e9);
        for (int pass = 0; pass < 2 && f.ok; ++pass) {
            for (size_t t = 0; t < tilePx.size(); ++t) change[t] = FitResidual(f, early, late, nullptr, tilePx[t]);
            if (pass == 1) break;
            std::vector<double> sorted = change;
            std::sort(sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            std::vector<size_t> good;
            for (size_t t = 0; t < tilePx.size(); ++t) if (change[t] <= median) good.insert(good.end(), tilePx[t].begin(), tilePx[t].end());
            const ToneFit f2 = FitTone(early, late, nullptr, good);
            if (f2.ok) f = f2;
        }
        *fitOut = f;
        return change;
    };
    ToneFit fitP, fitN;
    const std::vector<double> changeP = tileChange(ep, lp, &fitP);
    const std::vector<double> changeN = tileChange(en, ln, &fitN);
    // References: early and exposure-matched late frames averaged.
    std::vector<float> rp(n), rn(n);
    for (size_t i = 0; i < n; ++i) {
        rp[i] = static_cast<float>(0.5 * (ep[i] + (fitP.ok ? fitP.Tone(lp[i]) : lp[i])));
        rn[i] = static_cast<float>(0.5 * (en[i] + (fitN.ok ? fitN.Tone(ln[i]) : ln[i])));
    }

    std::vector<char> still(tilePx.size(), 0);
    int separable = 0, stillCount = 0, movedPlus = 0, movedMinus = 0;
    for (size_t t = 0; t < tilePx.size(); ++t) {
        double s = 0.0;
        for (size_t i : tilePx[t]) { const double d = rp[i] - rn[i]; s += d * d; }
        if (std::sqrt(s / tilePx[t].size()) < kMinSeparation) continue;
        ++separable;
        const bool okp = changeP[t] <= kMaxStillChange, okn = changeN[t] <= kMaxStillChange;
        movedPlus += !okp;
        movedMinus += !okn;
        if (okp && okn) { still[t] = 1; ++stillCount; }
    }
    std::vector<size_t> px;   // pixel indices of the still tiles
    for (size_t t = 0; t < tilePx.size(); ++t) if (still[t]) px.insert(px.end(), tilePx[t].begin(), tilePx[t].end());
    const bool enoughStill = stillCount >= kMinStillTiles;

    std::vector<float> dPlus(n), dMinus(n);   // other - own, for a + frame and for a - frame
    for (size_t i = 0; i < n; ++i) { dPlus[i] = rn[i] - rp[i]; dMinus[i] = rp[i] - rn[i]; }

    // Residual of a held frame against its own reference with the same fit: the scene's own frame-to-frame noise
    // on the still tiles. An alternating frame may leave at most twice that (and at least kMaxResidual).
    auto heldResidual = [&](const Capture* c, const std::vector<float>& own, const std::vector<float>& d) {
        const ToneFit f = FitTone(c->luma, own, &d, px);
        return f.ok ? FitResidual(f, c->luma, own, &d, px) : 0.0;
    };
    double heldResid = 0.0;
    for (const Capture* c : P) heldResid = (std::max)(heldResid, heldResidual(c, rp, dPlus));
    for (const Capture* c : Q) heldResid = (std::max)(heldResid, heldResidual(c, rp, dPlus));
    for (const Capture* c : N) heldResid = (std::max)(heldResid, heldResidual(c, rn, dMinus));
    for (const Capture* c : R) heldResid = (std::max)(heldResid, heldResidual(c, rn, dMinus));
    const double residLimit = (std::max)(kMaxResidual, 2.0 * heldResid);

    std::vector<FrameResult> res;
    for (const Capture* a : A) {
        FrameResult r;
        // Pose: the one whose reference fits better through a tone curve (on the still pixels, or all if none).
        const std::vector<size_t>& cpx = px.empty() ? allPx : px;
        const ToneFit fp = FitTone(a->luma, rp, nullptr, cpx), fn = FitTone(a->luma, rn, nullptr, cpx);
        const double ep2 = fp.ok ? FitResidual(fp, a->luma, rp, nullptr, cpx) : 1e9;
        const double en2 = fn.ok ? FitResidual(fn, a->luma, rn, nullptr, cpx) : 1e9;
        r.cls = ep2 <= en2 ? '+' : '-';
        const std::vector<float>& own = r.cls == '+' ? rp : rn;
        const std::vector<float>& d = r.cls == '+' ? dPlus : dMinus;
        const ToneFit f = FitTone(a->luma, own, &d, px);
        if (f.ok) {
            r.ghost = f.ghost;
            r.exposure = f.Tone(128.0) / 128.0;
            r.resid = FitResidual(f, a->luma, own, &d, px);
            // Per still tile, with this frame's tone curve: the local fraction of the other pose.
            // Only the SHAPES of the other pose count, not a uniform brightness shift within the tile: both the
            // leftover (A - tone(own)) and the other pose's difference d have their tile means removed first. A local
            // brightness change (bloom, centre-weighted exposure) would otherwise read as ghost wherever the other
            // pose happens to be darker or brighter -- the first version 3 runs flagged plain sky at the screen centre
            // this way. Tiles where the other pose has little structure (centred d below kMinTileStructure) can't
            // show a shaped ghost and are skipped.
            int ghosted = 0, measured = 0;
            double worst = 0.0;
            for (size_t t = 0; t < tilePx.size(); ++t) {
                if (!still[t]) continue;
                double mr = 0.0, md = 0.0;
                for (size_t i : tilePx[t]) { mr += a->luma[i] - f.Tone(own[i]); md += d[i]; }
                mr /= tilePx[t].size();
                md /= tilePx[t].size();
                double tn = 0.0, td = 0.0;
                for (size_t i : tilePx[t]) {
                    const double rc = a->luma[i] - f.Tone(own[i]) - mr, dc = d[i] - md;
                    tn += rc * dc;
                    td += dc * dc;
                }
                if (std::sqrt(td / tilePx[t].size()) < kMinTileStructure) continue;
                ++measured;
                const double tw = std::fabs(tn / td);
                worst = (std::max)(worst, tw);
                if (tw > kTileGhost) { ++ghosted; r.ghostedTiles.emplace_back(t, tw); }
            }
            r.ghostedFrac = measured ? static_cast<double>(ghosted) / measured : 0.0;
            r.tileGhostMax = worst;
            r.clean = std::fabs(r.ghost) <= kMaxGhost && r.ghostedFrac <= kMaxGhostedTiles && r.resid <= residLimit;
        }
        res.push_back(r);
    }

    bool alternates = true;
    std::string pattern;
    for (size_t i = 0; i < res.size(); ++i) {
        pattern += res[i].cls;
        if (i > 0 && res[i].cls == res[i - 1].cls) alternates = false;
    }
    int clean = 0;
    double worstGhost = 0.0, worstFrac = 0.0, worstResid = 0.0;
    for (const FrameResult& r : res) {
        if (r.clean) ++clean;
        if (!std::isnan(r.ghost)) worstGhost = (std::max)(worstGhost, std::fabs(r.ghost));
        if (!std::isnan(r.ghostedFrac)) worstFrac = (std::max)(worstFrac, r.ghostedFrac);
        if (!std::isnan(r.resid)) worstResid = (std::max)(worstResid, r.resid);
    }
    // A frame that isn't clean is a failure whatever the pattern; "did not alternate" is only reported as INVALID
    // when every frame is otherwise clean (then the test simply didn't exercise the flip).
    const bool allClean = clean == static_cast<int>(res.size());
    const bool pass = enoughStill && alternates && allClean;
    const char* verdict = !enoughStill ? "INVALID (too few still areas -- the scene moved during the test)"
        : !allClean ? "FAIL (ghosting or smearing)"
        : !alternates ? "INVALID (the poses did not alternate frame by frame)"
        : "PASS (clean)";

    const std::string stamp = TimeStamp();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    o << "Ghost test " << stamp << "\n"
      << "render_mode=" << RenderModeName(GetRenderMode()) << "\n"
      << "thumbnail=" << g.tw << "x" << g.th << " (backbuffer / " << g.factor << "), tiles " << nx << "x" << ny << " of " << kTile << " px\n"
      << "verdict=" << verdict << "\n"
      << "still_tiles=" << stillCount << "   (of " << separable << " where the poses differ by >= " << kMinSeparation
      << "; need >= " << kMinStillTiles << "; moved: +60 " << movedPlus << ", -60 " << movedMinus << ")\n"
      << "pattern=" << pattern << (alternates ? "   (alternates every frame)" : "   (NOT strictly alternating -- a pose repeated or was skipped)") << "\n"
      << "clean_frames=" << clean << "/" << res.size() << "\n"
      << "worst_ghost=" << worstGhost << " (limit " << kMaxGhost << ")  worst_ghosted_tiles=" << worstFrac << " (limit "
      << kMaxGhostedTiles << ", tile limit " << kTileGhost << ")  worst_residual=" << worstResid << " (limit " << residLimit
      << " = max(" << kMaxResidual << ", 2 x held-frame residual " << heldResid << "))\n\n"
      << "frame,present,class,ghost,ghosted_tiles,tile_ghost_max,residual,exposure_mid,clean\n";
    for (size_t i = 0; i < res.size(); ++i) {
        const FrameResult& r = res[i];
        o << "A" << i << ',' << A[i]->presentFrame << ',' << r.cls << ',' << r.ghost << ',' << r.ghostedFrac << ','
          << r.tileGhostMax << ',' << r.resid << ',' << r.exposure << ',' << (r.clean ? 1 : 0) << "\n";
    }
    // Camera movement during the test (the struct at each capture): position travel from the first capture, and for
    // the held stretches the rotation spread within each pose (early vs late). The alternating frames are left out of
    // the rotation figure: the struct then holds the latest write, not the pose the captured image was rendered with.
    auto angleDeg = [](const float a[4], const float b[4]) {
        double d = std::fabs(static_cast<double>(a[0]) * b[0] + static_cast<double>(a[1]) * b[1]
            + static_cast<double>(a[2]) * b[2] + static_cast<double>(a[3]) * b[3]);
        return 2.0 * std::acos((std::min)(1.0, d)) * 57.29578;
    };
    double travel = 0.0, rotPlus = 0.0, rotMinus = 0.0;
    const Capture* first = nullptr;
    for (const Capture& c : g.caps) {
        if (!c.havePose) continue;
        if (!first) first = &c;
        const double dx = c.pose.p[0] - first->pose.p[0], dy = c.pose.p[1] - first->pose.p[1], dz = c.pose.p[2] - first->pose.p[2];
        travel = (std::max)(travel, std::sqrt(dx * dx + dy * dy + dz * dz));
        for (const Capture& o2 : g.caps) {
            if (!o2.havePose) continue;
            const bool plus = (c.tag == 'P' || c.tag == 'Q') && (o2.tag == 'P' || o2.tag == 'Q');
            const bool minus = (c.tag == 'N' || c.tag == 'R') && (o2.tag == 'N' || o2.tag == 'R');
            if (plus) rotPlus = (std::max)(rotPlus, angleDeg(c.pose.q, o2.pose.q));
            if (minus) rotMinus = (std::max)(rotMinus, angleDeg(c.pose.q, o2.pose.q));
        }
    }
    o << "\ncamera_travel_cm=" << travel << "   (largest distance of the camera from its position at the first capture)\n"
      << "camera_rotation_spread_deg: +60 " << rotPlus << ", -60 " << rotMinus << "   (within each held pose, early vs late)\n";
    o << "\ncaptures (tag,present,t_ms,cam_x,cam_y,cam_z,qx,qy,qz,qw):\n";
    for (const Capture& c : g.caps) {
        o << c.tag << ',' << c.presentFrame << ',' << c.tMs;
        if (c.havePose) {
            o << ',' << c.pose.p[0] << ',' << c.pose.p[1] << ',' << c.pose.p[2];
            o.precision(5);
            o << ',' << c.pose.q[0] << ',' << c.pose.q[1] << ',' << c.pose.q[2] << ',' << c.pose.q[3];
            o.precision(3);
        }
        o << "\n";
    }
    // Where the local ghosts are (first frame of each pose), as tile column,row (0,0 = top left) and local ghost.
    for (char want : { '+', '-' }) {
        for (size_t i = 0; i < res.size(); ++i) {
            if (res[i].cls != want) continue;
            o << "\nghosted tiles in A" << i << " (" << want << "60), col,row=ghost:";
            if (res[i].ghostedTiles.empty()) o << " none";
            for (const auto& gt : res[i].ghostedTiles) {
                // Brightest reference pixel in the tile (own pose): near 255 = clipped highlight.
                float refMax = 0.0f;
                const std::vector<float>& own = want == '+' ? rp : rn;
                for (size_t k : tilePx[gt.first]) refMax = (std::max)(refMax, own[k]);
                o << "  " << gt.first % nx << ',' << gt.first / nx << '=' << gt.second << " (ref max " << refMax << ')';
            }
            break;
        }
    }
    o << "\n\ncontact sheet rows: [P0 P1 P2 N0 N1 N2] [A0..A5] [A6..A11] [Q0 Q1 Q2 R0 R1 R2]\n"
      << "  P/Q = held +60 early/late, N/R = held -60 early/late. Bar on top of each A frame: blue = classified +60,\n"
      << "  orange = -60; red frame border = not clean; yellow squares = ghosted tiles. Moving tiles (excluded) are\n"
      << "  dimmed on P0.\n";
    o << "\n" << g.settingsReport << "\n\ngame_settings=" << FrameLogReadGameSettings() << "\n";

    const std::string txtName = "vrmod_ghost_" + stamp + ".txt";
    std::ofstream(txtName, std::ios::binary) << o.str();

    // Contact sheet.
    const uint32_t gap = 4;
    const uint32_t rows = 4;
    const uint32_t sw = kSheetColumns * (g.tw + gap) + gap, sh = rows * (g.th + gap) + gap;
    std::vector<uint8_t> sheet(static_cast<size_t>(sw) * sh * 4, 40);
    auto put = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t gg, uint8_t b) {
        uint8_t* p = &sheet[(static_cast<size_t>(y) * sw + x) * 4];
        p[0] = r; p[1] = gg; p[2] = b; p[3] = 255;
    };
    auto blit = [&](const Capture& c, uint32_t col, uint32_t row, int bar, bool redBorder, bool dimMoving) {
        const uint32_t x0 = gap + col * (g.tw + gap), y0 = gap + row * (g.th + gap);
        for (uint32_t y = 0; y < g.th; ++y) {
            memcpy(&sheet[(static_cast<size_t>(y0 + y) * sw + x0) * 4], &c.rgba[static_cast<size_t>(y) * g.tw * 4], static_cast<size_t>(g.tw) * 4);
            if (!dimMoving) continue;
            for (uint32_t x = 0; x < g.tw; ++x) {
                const uint32_t tx = x / kTile, ty = y / kTile;
                if (tx < nx && ty < ny && still[ty * nx + tx]) continue;
                uint8_t* p = &sheet[(static_cast<size_t>(y0 + y) * sw + x0 + x) * 4];
                p[0] = static_cast<uint8_t>(p[0] / 4); p[1] = static_cast<uint8_t>(p[1] / 4); p[2] = static_cast<uint8_t>(p[2] / 4);
            }
        }
        if (bar != 0) {
            for (uint32_t y = y0; y < y0 + 6; ++y) for (uint32_t x = x0; x < x0 + g.tw; ++x) {
                if (bar > 0) put(x, y, 40, 110, 255); else put(x, y, 255, 150, 30);
            }
        }
        if (redBorder) {
            for (uint32_t x = x0; x < x0 + g.tw; ++x) for (uint32_t t = 0; t < 3; ++t) { put(x, y0 + t, 255, 0, 0); put(x, y0 + g.th - 1 - t, 255, 0, 0); }
            for (uint32_t y = y0; y < y0 + g.th; ++y) for (uint32_t t = 0; t < 3; ++t) { put(x0 + t, y, 255, 0, 0); put(x0 + g.tw - 1 - t, y, 255, 0, 0); }
        }
    };
    uint32_t col = 0;
    for (const Capture* c : P) { blit(*c, col, 0, 0, false, col == 0); ++col; }
    for (const Capture* c : N) blit(*c, col++, 0, 0, false, false);
    for (size_t i = 0; i < A.size(); ++i) {
        const uint32_t ac = static_cast<uint32_t>(i % kSheetColumns), ar = 1 + static_cast<uint32_t>(i / kSheetColumns);
        blit(*A[i], ac, ar, res[i].cls == '+' ? 1 : -1, !res[i].clean, false);
        // Yellow outline around each ghosted tile.
        const uint32_t x0 = gap + ac * (g.tw + gap), y0 = gap + ar * (g.th + gap);
        for (const auto& gt : res[i].ghostedTiles) {
            const uint32_t tx = static_cast<uint32_t>(gt.first % nx), ty = static_cast<uint32_t>(gt.first / nx);
            const uint32_t bx = x0 + tx * kTile, by = y0 + ty * kTile;
            for (uint32_t k = 0; k < static_cast<uint32_t>(kTile); ++k) for (uint32_t t = 0; t < 2; ++t) {
                put(bx + k, by + t, 255, 230, 0); put(bx + k, by + kTile - 1 - t, 255, 230, 0);
                put(bx + t, by + k, 255, 230, 0); put(bx + kTile - 1 - t, by + k, 255, 230, 0);
            }
        }
    }
    col = 0;
    for (const Capture* c : Q) blit(*c, col++, 3, 0, false, false);
    for (const Capture* c : R) blit(*c, col++, 3, 0, false, false);
    const std::string bmpName = "vrmod_ghost_" + stamp + ".bmp";
    const bool bmpOk = WriteBmp32(bmpName, sw, sh, sheet.data(), sw * 4, true);

    std::ostringstream l;
    l.setf(std::ios::fixed);
    l.precision(3);
    l << "GHOST: " << verdict << " | pattern " << pattern << " | clean " << clean << "/" << res.size()
      << " | worst ghost " << worstGhost << ", ghosted tiles " << worstFrac << ", residual " << worstResid << " (limit " << residLimit << ")"
      << " | still tiles " << stillCount << "/" << separable << " | camera moved " << travel << " cm -> " << txtName << (bmpOk ? ", " + bmpName : ", (sheet FAILED)");
    modlog::Line(l.str());
    PlayCue(pass ? Cue::Success : Cue::Failure);
}

bool CtrlGPressed() {
    static bool wasDown = false;
    const bool down = (GetAsyncKeyState('G') & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
}

} // namespace

bool GhostTestBlackout() {
    return g.running || g.blackoutTail > 0;
}

std::string TemporalSettingsReport() {
    const std::string block = FrameLogReadGameSettings();
    std::ostringstream o;
    auto item = [&](const char* label, const std::string& value, const std::string& want, const char* note) {
        o << "\n  " << label << " = " << (value.empty() ? "(not found)" : value);
        if (!want.empty()) o << (value == want ? "  ok" : "  <-- want " + want);
        if (note && *note) o << "  (" << note << ")";
    };
    const bool mono = GetRenderMode() == RenderMode::Mono;
    o << "Temporal-effects check (from the game's own log; [render] mode=" << RenderModeName(GetRenderMode()) << "):";
    item("Stereoscopic3D", SettingValue(block, "Stereoscopic3D"), mono ? "0" : "1", mono ? "mono mode needs it off" : "sbs mode needs it on");
    item("DLSS", SettingValue(block, "DLSS"), "Off", "temporal");
    item("XeSS", SettingValue(block, "XeSS"), "Off", "temporal");
    item("AA", SettingValue(block, "AA"), "1", "1 = SMAA (confirmed in the menu); not TAA, not SMAA T2x");
    item("MotionBlur", SettingValue(block, "MotionBlur"), "0", "blends along motion");
    item("DOF", SettingValue(block, "DOF"), "0", "");
    item("ScreenFX", SettingValue(block, "ScreenFX"), "0", "screen effects: film grain / chromatic aberration");
    item("SSR", SettingValue(block, "SSR"), "", "screen-space reflections; turn off if ghosting remains");
    item("AO", SettingValue(block, "AO"), "", "ambient occlusion; turn off (or try another mode) if ghosting remains");
    item("RT", SettingValue(block, "RT"), "0", "ray-traced shadows use a temporal denoiser");
    const bool opti = GetFileAttributesW(L"OptiScaler.ini") != INVALID_FILE_ATTRIBUTES;
    o << "\n  OptiScaler = " << (opti ? "installed (OptiScaler.ini) -- its upscaler / NR must be off for this test" : "not found");
    modlog::Line(o.str());
    return o.str();
}

void GhostTestOnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) {
    if (!device || !queue || !backbuffer) return;
    const bool pressed = CtrlGPressed();

    if (!g.running) {
        if (g.blackoutTail > 0) --g.blackoutTail;
        if (!pressed) return;
        if (MenuScreenActive() || !IsHeadTrackingEnabled()) {
            modlog::Line("GHOST: start it during gameplay with head tracking on (no menu, map or video).");
            PlayCue(Cue::Failure);
            return;
        }
        if (!Setup(device, backbuffer)) {
            PlayCue(Cue::Failure);
            return;
        }
        g.running = true;
        g.step = 0;
        g.settle = 0;
        g.captured = 0;
        g.caps.clear();
        g.settingsReport = TemporalSettingsReport();
        HeadTrackingFreeze(true);
        // Third-person camera for the test: the first-person camera sits in Lara's head, so +-60 degrees looks into
        // her hair and shoulders, which move. Restored afterwards (30 s safety timeout).
        CameraReasonBegin(ThirdPersonReason::Diagnostic, 30.0);
        FrameLogForceFlipMode(kSteps[0].mode);
        modlog::Line("GHOST: test started (~4 s; headset view black, head tracking frozen, third-person camera; keep the mouse still). Thumbnails "
            + std::to_string(g.tw) + "x" + std::to_string(g.th) + ".");
        PlayCue(Cue::On);
        return;
    }

    if (pressed) { Abort("cancelled with Ctrl+G"); return; }
    if (MenuScreenActive() || !IsHeadTrackingEnabled()) { Abort("a menu opened or head tracking went off"); return; }

    if (g.settle < kSettlePresents) {
        ++g.settle;
        return;
    }
    Capture c;
    c.tag = kSteps[g.step].tag;
    c.presentFrame = FrameLogCurrentFrame();
    c.tMs = NowMs();
    c.havePose = CameraReadStructPose(&c.pose);
    if (!CaptureFrame(queue, backbuffer, &c)) { Abort("readback failed or the resolution changed"); return; }
    g.caps.push_back(std::move(c));
    if (++g.captured < kSteps[g.step].captures) return;

    g.captured = 0;
    g.settle = 0;
    if (++g.step < kStepCount) {
        FrameLogForceFlipMode(kSteps[g.step].mode);
        return;
    }
    Finish();
    Analyse();
    g.caps.clear();
}

#ifdef GHOST_TEST_HARNESS
// Offline check of the analysis: two random textured "poses" X (+60) and Y (-60) with per-frame noise.
//   variant 0: alternating frames blended with `blend` of the opposite pose
//   variant 1: clean, but the left 40% of the picture changes between phases (like hair) and the alternating frames
//              are 10% darker + offset (auto-exposure)
//   variant 2: alternating frames smeared (horizontal 13-px box blur, like motion blur)
//   variant 3: the whole picture changes between phases (nothing still)
void GhostTestSynthetic(double blend, double noise, unsigned seed, int variant) {
    g.tw = 384; g.th = 256; g.factor = 1;
    g.caps.clear();
    g.settingsReport = "(synthetic)";
    uint32_t s = seed;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0; };
    const size_t n = static_cast<size_t>(g.tw) * g.th;
    std::vector<float> X(n), Y(n);
    // Large shapes plus fine texture, like a real scene (sky/cliff vs ground).
    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i % g.tw), y = static_cast<double>(i / g.tw);
        X[i] = static_cast<float>(110 + 50 * std::sin(x / 40.0) * std::cos(y / 55.0) + 40 * (rnd() - 0.5));
        Y[i] = static_cast<float>(90 + 45 * std::cos(x / 23.0 + 1.0) * std::sin(y / 31.0) + 40 * (rnd() - 0.5));
    }
    auto moving = [&](size_t i) {
        if (variant == 3) return true;
        return variant == 1 && (i % g.tw) < g.tw * 2 / 5;
    };
    auto make = [&](char tag, const std::vector<float>& base, const std::vector<float>* other, double b, double gain, double off, int phase) {
        Capture c;
        c.tag = tag;
        c.luma.resize(n);
        c.rgba.assign(n * 4, 255);
        std::vector<double> v(n);
        for (size_t i = 0; i < n; ++i) {
            double bv = base[i];
            if (moving(i)) bv = 40 + 150 * std::fabs(std::sin(0.37 * (i % 997) + 1.7 * phase));   // different per phase
            v[i] = bv + noise * (rnd() - 0.5) * 3.4641;   // uniform with sd = noise
            if (other) v[i] = (1.0 - b) * v[i] + b * (*other)[i];
            v[i] = gain * v[i] + off;
        }
        if (variant == 2 && tag == 'A') {
            std::vector<double> sm(n);
            for (uint32_t y = 0; y < g.th; ++y) for (uint32_t x = 0; x < g.tw; ++x) {
                double sum = 0; int k = 0;
                for (int dx = -6; dx <= 6; ++dx) {
                    const int xx = static_cast<int>(x) + dx;
                    if (xx < 0 || xx >= static_cast<int>(g.tw)) continue;
                    sum += v[static_cast<size_t>(y) * g.tw + xx]; ++k;
                }
                sm[static_cast<size_t>(y) * g.tw + x] = sum / k;
            }
            v = sm;
        }
        for (size_t i = 0; i < n; ++i) {
            c.luma[i] = static_cast<float>(v[i]);
            c.rgba[i * 4] = c.rgba[i * 4 + 1] = c.rgba[i * 4 + 2] = static_cast<uint8_t>((std::min)(255.0, (std::max)(0.0, v[i])));
        }
        g.caps.push_back(std::move(c));
    };
    const double ag = variant == 1 ? 0.9 : 1.0, ao = variant == 1 ? 8.0 : 0.0;
    for (int i = 0; i < 3; ++i) make('P', X, nullptr, 0, 1, 0, 0);
    for (int i = 0; i < 3; ++i) make('N', Y, nullptr, 0, 1, 0, 1);
    for (int i = 0; i < 12; ++i) make('A', i % 2 ? Y : X, i % 2 ? &X : &Y, blend, ag, ao, 2);
    for (int i = 0; i < 3; ++i) make('Q', X, nullptr, 0, 1, 0, 3);
    for (int i = 0; i < 3; ++i) make('R', Y, nullptr, 0, 1, 0, 4);
    Analyse();
}

// Re-analyses an old contact sheet (layout P0-2 N0-2 / A0-5 / A6-11 / Q0-1, 549x488 thumbnails). It has no late
// -60 frames, so N stands in for R (the -60 still check is then trivially passed). The classification bars and
// borders drawn on the sheet are blanked in every frame so they add no difference.
void GhostTestFromSheet(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    const uint32_t off = *reinterpret_cast<uint32_t*>(&file[10]);
    const int sw = *reinterpret_cast<int32_t*>(&file[18]), sh = *reinterpret_cast<int32_t*>(&file[22]);
    g.tw = 549; g.th = 488; g.factor = 11;
    g.caps.clear();
    g.settingsReport = std::string("(re-analysis of ") + path + ")";
    auto thumb = [&](char tag, int col, int row) {
        Capture c;
        c.tag = tag;
        const size_t n = static_cast<size_t>(g.tw) * g.th;
        c.luma.assign(n, 0.0f);
        c.rgba.assign(n * 4, 255);
        const int x0 = 4 + col * (static_cast<int>(g.tw) + 4), y0 = 4 + row * (static_cast<int>(g.th) + 4);
        for (uint32_t y = 0; y < g.th; ++y) for (uint32_t x = 0; x < g.tw; ++x) {
            const size_t i = static_cast<size_t>(y) * g.tw + x;
            const bool drawn = y < 8 || y >= g.th - 3 || x < 3 || x >= g.tw - 3;
            const uint8_t* p = &file[off + (static_cast<size_t>(sh - 1 - (y0 + static_cast<int>(y))) * sw + x0 + x) * 4];
            const float v = drawn ? 0.0f : 0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2];
            c.luma[i] = v;
            c.rgba[i * 4] = drawn ? 0 : p[2]; c.rgba[i * 4 + 1] = drawn ? 0 : p[1]; c.rgba[i * 4 + 2] = drawn ? 0 : p[0];
        }
        g.caps.push_back(std::move(c));
    };
    // Version 2 sheets have late -60 frames in row 4, columns 4-6 (version 1 left that area background grey), and
    // dim P0's moving tiles, so P0 is skipped there.
    const int probeX = 4 + 4 * (static_cast<int>(g.tw) + 4) + static_cast<int>(g.tw) / 2;
    const int probeY = 4 + 3 * (static_cast<int>(g.th) + 4) + static_cast<int>(g.th) / 2;
    const uint8_t* probe = &file[off + (static_cast<size_t>(sh - 1 - probeY) * sw + probeX) * 4];
    const bool v2 = !(probe[0] == 40 && probe[1] == 40 && probe[2] == 40);
    std::printf("  layout: %s\n", v2 ? "version 2 (P1-2, N0-2, A0-11, Q0-2, R0-2)" : "version 1 (P0-2, N0-2, A0-11, Q0-1; R = N)");
    for (int i = v2 ? 1 : 0; i < 3; ++i) thumb('P', i, 0);
    for (int i = 0; i < 3; ++i) thumb('N', 3 + i, 0);
    for (int i = 0; i < 12; ++i) thumb('A', i % 6, 1 + i / 6);
    for (int i = 0; i < (v2 ? 3 : 2); ++i) thumb('Q', i, 3);
    for (int i = 0; i < 3; ++i) thumb('R', 3 + i, v2 ? 3 : 0);
    // Version 3 sheets outline ghosted tiles in yellow (255,230,0) on the A frames: blank those pixels in every frame.
    std::vector<char> yellow(static_cast<size_t>(g.tw) * g.th, 0);
    for (int i = 0; i < 12; ++i) {
        const int x0 = 4 + (i % 6) * (static_cast<int>(g.tw) + 4), y0 = 4 + (1 + i / 6) * (static_cast<int>(g.th) + 4);
        for (uint32_t y = 0; y < g.th; ++y) for (uint32_t x = 0; x < g.tw; ++x) {
            const uint8_t* p = &file[off + (static_cast<size_t>(sh - 1 - (y0 + static_cast<int>(y))) * sw + x0 + x) * 4];
            if (p[2] == 255 && p[1] == 230 && p[0] == 0) yellow[static_cast<size_t>(y) * g.tw + x] = 1;
        }
    }
    size_t blanked = 0;
    for (size_t k = 0; k < yellow.size(); ++k) {
        if (!yellow[k]) continue;
        ++blanked;
        for (Capture& c : g.caps) c.luma[k] = 0.0f;
    }
    std::printf("  blanked %zu outline pixels\n", blanked);
    Analyse();
}
#endif

} // namespace vrmod
