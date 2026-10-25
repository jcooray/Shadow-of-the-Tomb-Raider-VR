#include "pairing_test.h"

#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "audio_cue.h"
#include "camera_interceptor.h"
#include "frame_log.h"
#include "head_tracking.h"
#include "logger.h"
#include "openxr_integration.h"
#include "perf_stats.h"
#include "settings.h"

namespace vrmod {

namespace {

// Patches: a 4x4 grid of 32x32-px squares, centred in the 16 cells of the backbuffer. One readback slot holds all
// 16 (row pitch padded to 256 bytes as D3D12 requires; each patch starts on a 512-byte boundary).
constexpr int kGrid = 4;
constexpr int kFeat = kGrid * kGrid;
constexpr UINT kPatch = 32;
constexpr UINT kPatchPitch = 256;
constexpr UINT64 kPatchBytes = static_cast<UINT64>(kPatchPitch) * kPatch;
constexpr UINT64 kSlotBytes = kPatchBytes * kFeat;
constexpr int kSlots = 8;             // copies in flight; results are read back up to 8 Presents later

constexpr int kMaxN = 12;             // candidate offsets 0..12 game frames
constexpr int kWindow = 8;            // per-frame N from +-8 neighbouring frames
constexpr double kAmbiguous = 0.5;    // |score| below this: the frame looks like neither pose cleanly

enum Phase : uint8_t { Settle = 0, HoldPlus = 1, HoldMinus = 2, Random = 3 };
const char* const kPhaseNames[] = { "settle", "hold+60", "hold-60", "random" };

struct Segment {
    const char* name;
    int framesAhead;
    bool halfRate;
    int stallEvery;   // inject a game-thread stall every this many random-flip frames (0 = never)
    int stallMs;
};
// framesAhead 0 / 3 made no difference in two runs (2026-09-24), so the segments now vary game-thread timing instead:
// injected stalls (a Sleep in the camera hook) force the "game thread late" case and hitches.
const Segment kSegments[] = {
    { "default (framesAhead 1)", 1, false, 0, 0 },
    { "half rate (framesAhead 1, game at half the display rate)", 1, true, 0, 0 },
    { "20 ms game-thread stall every 50 frames", 1, false, 50, 20 },
    { "60 ms game-thread hitch every 100 frames", 1, false, 100, 60 },
};
constexpr int kSegmentCount = sizeof(kSegments) / sizeof(kSegments[0]);
constexpr int kSettle = 60, kHold = 40, kHoldUse = 20, kRandomFrames = 2500;

struct Rec {
    uint8_t seg = 0, phase = 0;
    uint64_t present = 0, gameFrame = 0;
    double tMs = 0.0;
    float f[kFeat] = {};
    bool have = false;
};

struct Slot {
    ID3D12CommandAllocator* alloc = nullptr;
    UINT64 fenceValue = 0;
    long long rec = -1;   // record waiting for this slot's data
};

struct State {
    bool running = false;
    int seg = 0;
    int phase = Settle;
    int inPhase = 0;
    int blackoutTail = 0;
    uint64_t seed = 0;
    XrPipelineConfig saved;
    std::vector<Rec> recs;
    ID3D12Resource* readback = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE evt = nullptr;
    UINT64 fenceValue = 0;
    Slot slots[kSlots];
    bool bgra = false;
    uint64_t presents = 0;
};
State g;

int PhaseLength(int phase) {
    switch (phase) {
    case Settle: return kSettle;
    case HoldPlus: case HoldMinus: return kHold;
    default: return kRandomFrames;
    }
}

FlipMode PhaseFlip(int phase) {
    switch (phase) {
    case Settle: case HoldPlus: return FlipMode::HoldPlus;
    case HoldMinus: return FlipMode::HoldMinus;
    default: return FlipMode::Random;
    }
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void ReleaseResources() {
    for (Slot& s : g.slots) {
        if (s.alloc) { s.alloc->Release(); s.alloc = nullptr; }
        s.rec = -1;
        s.fenceValue = 0;
    }
    if (g.list) { g.list->Release(); g.list = nullptr; }
    if (g.fence) { g.fence->Release(); g.fence = nullptr; }
    if (g.readback) { g.readback->Release(); g.readback = nullptr; }
    if (g.evt) { CloseHandle(g.evt); g.evt = nullptr; }
}

bool Setup(ID3D12Device* device, ID3D12Resource* backbuffer) {
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    g.bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!rgba && !g.bgra) {
        modlog::Line("PAIRING: backbuffer format " + std::to_string(desc.Format) + " is not 8-bit RGBA/BGRA; can't measure.");
        return false;
    }
    if (desc.Width < kGrid * kPatch || desc.Height < kGrid * kPatch) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kSlotBytes * kSlots;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = g.evt
        && SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.readback)))
        && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
    for (Slot& s : g.slots) {
        ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)));
    }
    ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.slots[0].alloc, nullptr, IID_PPV_ARGS(&g.list)));
    if (!ok) {
        modlog::Line("PAIRING: could not create readback resources.");
        ReleaseResources();
        return false;
    }
    g.list->Close();
    g.fenceValue = 0;
    return true;
}

// Reads a finished slot's 16 patches into its record (mean luma per patch).
void Harvest(int si) {
    Slot& s = g.slots[si];
    if (s.rec < 0) return;
    void* mapped = nullptr;
    D3D12_RANGE range{ static_cast<SIZE_T>(kSlotBytes * si), static_cast<SIZE_T>(kSlotBytes * (si + 1)) };
    if (SUCCEEDED(g.readback->Map(0, &range, &mapped)) && mapped) {
        const uint8_t* base = static_cast<const uint8_t*>(mapped) + kSlotBytes * si;
        const int ri = g.bgra ? 2 : 0, bi = g.bgra ? 0 : 2;
        Rec& r = g.recs[static_cast<size_t>(s.rec)];
        for (int p = 0; p < kFeat; ++p) {
            double sum = 0.0;
            for (UINT y = 0; y < kPatch; ++y) {
                const uint8_t* row = base + kPatchBytes * p + static_cast<size_t>(y) * kPatchPitch;
                for (UINT x = 0; x < kPatch; ++x) {
                    const uint8_t* px = row + x * 4;
                    sum += 0.2126 * px[ri] + 0.7152 * px[1] + 0.0722 * px[bi];
                }
            }
            r.f[p] = static_cast<float>(sum / (kPatch * kPatch));
        }
        r.have = true;
        D3D12_RANGE none{ 0, 0 };
        g.readback->Unmap(0, &none);
    }
    s.rec = -1;
}

void HarvestCompleted(bool wait) {
    for (int si = 0; si < kSlots; ++si) {
        Slot& s = g.slots[si];
        if (s.rec < 0) continue;
        if (g.fence->GetCompletedValue() < s.fenceValue) {
            if (!wait) continue;
            g.fence->SetEventOnCompletion(s.fenceValue, g.evt);
            if (WaitForSingleObject(g.evt, 2000) != WAIT_OBJECT_0) { s.rec = -1; continue; }
        }
        Harvest(si);
    }
}

// Queues the copy of this Present's 16 patches into a free slot, for record `rec`.
bool QueueCopy(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, long long rec) {
    const int si = static_cast<int>(g.presents % kSlots);
    Slot& s = g.slots[si];
    if (s.rec >= 0) {   // the GPU is more than kSlots frames behind: wait for this slot (rare)
        if (g.fence->GetCompletedValue() < s.fenceValue) {
            g.fence->SetEventOnCompletion(s.fenceValue, g.evt);
            if (WaitForSingleObject(g.evt, 2000) != WAIT_OBJECT_0) return false;
        }
        Harvest(si);
    }
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    s.alloc->Reset();
    g.list->Reset(s.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = backbuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->ResourceBarrier(1, &b);
    for (int p = 0; p < kFeat; ++p) {
        const int gx = p % kGrid, gy = p / kGrid;
        const UINT x0 = static_cast<UINT>((gx + 0.5) * desc.Width / kGrid) - kPatch / 2;
        const UINT y0 = static_cast<UINT>((gy + 0.5) * desc.Height / kGrid) - kPatch / 2;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = kSlotBytes * si + kPatchBytes * p;
        dst.PlacedFootprint.Footprint = { desc.Format, kPatch, kPatch, 1, kPatchPitch };
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = backbuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{ x0, y0, 0, x0 + kPatch, y0 + kPatch, 1 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g.list->ResourceBarrier(1, &b);
    g.list->Close();
    ID3D12CommandList* lists[] = { g.list };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g.fence, ++g.fenceValue);
    s.fenceValue = g.fenceValue;
    s.rec = rec;
    return true;
}

// ---- analysis ----

// Features centred and scaled to unit length, so a change of exposure (gain/offset) doesn't move them.
bool Normalise(const float in[kFeat], double out[kFeat]) {
    double m = 0.0;
    for (int i = 0; i < kFeat; ++i) m += in[i];
    m /= kFeat;
    double n = 0.0;
    for (int i = 0; i < kFeat; ++i) { out[i] = in[i] - m; n += out[i] * out[i]; }
    n = std::sqrt(n);
    if (n < 1e-3) return false;
    for (int i = 0; i < kFeat; ++i) out[i] /= n;
    return true;
}

struct SegResult {
    int randomFrames = 0, valid = 0, ambiguous = 0, holdPlusBad = 0, holdMinusBad = 0, holdCount = 0;
    double separation = 0.0;
    int bestN = -1;
    double agree[kMaxN + 1] = {};
    // Same with the Present-count anchor: H = G at the segment's first frame + Presents since. Unlike G it doesn't
    // depend on how far ahead the game thread happens to be at each Present.
    double agreeH[kMaxN + 1] = {};
    int bestNH = -1;
    // Clamped Present count (the candidate pairing rule): S_k = clamp(S_{k-1} + Presents elapsed, G_k - 2, G_k - 1),
    // started at G - 2. Scored at N = 0 against S. corrections = Presents where the clamp moved S.
    double agreeClamp = 0.0;
    int clampUp = 0, clampDown = 0, clampMiss = 0;
    std::map<int, int> windowN;   // per-frame N histogram (-1 = undetermined)
    int mismatchBest = 0;
    int gameFrameRepeats = 0, gameFrameSkips = 0;   // Present-to-Present steps of the written game frame: 0 / >= 2
    double presentMs = 0.0;
    bool fixed = false;
};

void Analyse(bool aborted) {
    const std::string stamp = TimeStamp();
    std::vector<SegResult> results(kSegmentCount);
    std::vector<double> score(g.recs.size(), std::nan(""));
    std::vector<int> cls(g.recs.size(), 0), frameN(g.recs.size(), -2);

    for (int sg = 0; sg < kSegmentCount; ++sg) {
        SegResult& R = results[sg];
        // References: the last kHoldUse frames of each hold.
        double zp[kFeat] = {}, zn[kFeat] = {};
        int np = 0, nn = 0;
        std::vector<size_t> holdP, holdN, rnd;
        for (size_t i = 0; i < g.recs.size(); ++i) {
            const Rec& r = g.recs[i];
            if (r.seg != sg || !r.have) continue;
            if (r.phase == HoldPlus) holdP.push_back(i);
            else if (r.phase == HoldMinus) holdN.push_back(i);
            else if (r.phase == Random) rnd.push_back(i);
        }
        auto accumulate = [&](const std::vector<size_t>& idx, double* z, int* count) {
            const size_t from = idx.size() > static_cast<size_t>(kHoldUse) ? idx.size() - kHoldUse : 0;
            for (size_t k = from; k < idx.size(); ++k) {
                double v[kFeat];
                if (!Normalise(g.recs[idx[k]].f, v)) continue;
                for (int i = 0; i < kFeat; ++i) z[i] += v[i];
                ++*count;
            }
            for (int i = 0; i < kFeat; ++i) if (*count) z[i] /= *count;
        };
        accumulate(holdP, zp, &np);
        accumulate(holdN, zn, &nn);
        if (np == 0 || nn == 0) continue;
        double u[kFeat], mid[kFeat], uu = 0.0;
        for (int i = 0; i < kFeat; ++i) { u[i] = zp[i] - zn[i]; mid[i] = 0.5 * (zp[i] + zn[i]); uu += u[i] * u[i]; }
        R.separation = std::sqrt(uu);
        if (uu < 1e-6) continue;
        // score: +1 at the +60 reference, -1 at the -60 one.
        auto scoreOf = [&](size_t i) {
            double v[kFeat];
            if (!Normalise(g.recs[i].f, v)) return std::nan("");
            double s = 0.0;
            for (int k = 0; k < kFeat; ++k) s += (v[k] - mid[k]) * u[k];
            return 2.0 * s / uu;
        };
        for (size_t i : holdP) { const double s = scoreOf(i); score[i] = s; ++R.holdCount; if (!(s > kAmbiguous)) ++R.holdPlusBad; }
        for (size_t i : holdN) { const double s = scoreOf(i); score[i] = s; ++R.holdCount; if (!(s < -kAmbiguous)) ++R.holdMinusBad; }

        std::vector<size_t> valid;
        for (size_t i : rnd) {
            const double s = scoreOf(i);
            score[i] = s;
            ++R.randomFrames;
            if (std::isnan(s) || std::fabs(s) < kAmbiguous) { ++R.ambiguous; continue; }
            cls[i] = s > 0 ? 1 : -1;
            valid.push_back(i);
        }
        R.valid = static_cast<int>(valid.size());
        if (valid.empty()) continue;

        // Whole-segment agreement at each N.
        for (int n = 0; n <= kMaxN; ++n) {
            int match = 0;
            for (size_t i : valid) {
                const uint64_t gf = g.recs[i].gameFrame;
                if (gf >= static_cast<uint64_t>(n) && cls[i] == FrameLogRandomFlipSign(gf - n)) ++match;
            }
            R.agree[n] = static_cast<double>(match) / valid.size();
            if (R.bestN < 0 || R.agree[n] > R.agree[R.bestN]) R.bestN = n;
        }
        for (size_t i : valid) {
            const uint64_t gf = g.recs[i].gameFrame;
            if (!(gf >= static_cast<uint64_t>(R.bestN) && cls[i] == FrameLogRandomFlipSign(gf - R.bestN))) ++R.mismatchBest;
        }
        // Per-frame N: the offset that explains the frames around it (a unique best with >= 90 % agreement).
        for (size_t k = 0; k < valid.size(); ++k) {
            const size_t lo = k >= static_cast<size_t>(kWindow) ? k - kWindow : 0;
            const size_t hi = (std::min)(valid.size(), k + kWindow + 1);
            int best = -1, bestMatch = -1, second = -1;
            for (int n = 0; n <= kMaxN; ++n) {
                int match = 0;
                for (size_t j = lo; j < hi; ++j) {
                    const uint64_t gf = g.recs[valid[j]].gameFrame;
                    if (gf >= static_cast<uint64_t>(n) && cls[valid[j]] == FrameLogRandomFlipSign(gf - n)) ++match;
                }
                if (match > bestMatch) { second = bestMatch; bestMatch = match; best = n; }
                else if (match > second) second = match;
            }
            const int count = static_cast<int>(hi - lo);
            const int n = (bestMatch >= 0.9 * count && bestMatch > second) ? best : -1;
            frameN[valid[k]] = n;
            ++R.windowN[n];
        }
        // How the written game frame advances between Presents, and the Present interval.
        for (size_t k = 1; k < rnd.size(); ++k) {
            const Rec& a = g.recs[rnd[k - 1]];
            const Rec& b = g.recs[rnd[k]];
            const uint64_t step = b.gameFrame - a.gameFrame;
            if (step == 0) ++R.gameFrameRepeats;
            if (step >= 2) ++R.gameFrameSkips;
        }
        if (rnd.size() > 1) R.presentMs = (g.recs[rnd.back()].tMs - g.recs[rnd.front()].tMs) / (rnd.size() - 1);
        const int atBest = R.windowN.count(R.bestN) ? R.windowN[R.bestN] : 0;
        R.fixed = R.agree[R.bestN] >= 0.98 && atBest >= 0.98 * R.valid;
        {
            const uint64_t g0 = g.recs[valid.front()].gameFrame, p0 = g.recs[valid.front()].present;
            for (int n = 0; n <= kMaxN; ++n) {
                int match = 0;
                for (size_t i : valid) {
                    const uint64_t h = g0 + (g.recs[i].present - p0);
                    if (h >= static_cast<uint64_t>(n) && cls[i] == FrameLogRandomFlipSign(h - n)) ++match;
                }
                R.agreeH[n] = static_cast<double>(match) / valid.size();
                if (R.bestNH < 0 || R.agreeH[n] > R.agreeH[R.bestNH]) R.bestNH = n;
            }
        }
        {
            long long S = -1;
            uint64_t prevPresent = 0;
            int total = 0, match = 0;
            for (size_t i : rnd) {
                const Rec& r = g.recs[i];
                const long long G = static_cast<long long>(r.gameFrame);
                long long pred = S < 0 ? G - 2 : S + static_cast<long long>(r.present - prevPresent);
                if (pred < G - 2) { pred = G - 2; ++R.clampUp; }
                if (pred > G - 1) { pred = G - 1; ++R.clampDown; }
                S = pred;
                prevPresent = r.present;
                if (cls[i] == 0 || S < 0) continue;
                ++total;
                if (cls[i] == FrameLogRandomFlipSign(static_cast<uint64_t>(S))) ++match; else ++R.clampMiss;
            }
            R.agreeClamp = total ? static_cast<double>(match) / total : 0.0;
        }
    }

    // Verdict.
    bool allFixed = true, sameN = true;
    int firstN = -1, measured = 0, totalRandom = 0;
    for (const SegResult& R : results) {
        totalRandom += R.randomFrames;
        if (R.valid == 0) { allFixed = false; continue; }
        ++measured;
        allFixed = allFixed && R.fixed;
        if (firstN < 0) firstN = R.bestN; else if (R.bestN != firstN) sameN = false;
    }
    std::string verdict;
    if (aborted) verdict = "INCOMPLETE (aborted) -- results below cover the segments that finished";
    else if (measured < kSegmentCount) verdict = "INVALID (a segment had no usable frames -- see separation / ambiguous)";
    else if (allFixed && sameN) verdict = "FIXED N = " + std::to_string(firstN) + " in every segment";
    else if (allFixed) verdict = "FIXED N within each segment, but it depends on the pipeline setting (see per segment)";
    else verdict = "VARIABLE N (at least one segment has no single N that explains >= 98 % of frames)";
    {
        bool hFixed = measured == kSegmentCount;
        for (const SegResult& R : results) if (R.valid == 0 || R.agreeH[R.bestNH] < 0.99) hFixed = false;
        verdict += hFixed ? " | Present-count anchor: >= 99 % in every segment" : " | Present-count anchor: below 99 % in some segment";
        int misses = 0, total = 0;
        double worst = 1.0;
        for (const SegResult& R : results) { misses += R.clampMiss; total += R.valid; if (R.valid) worst = (std::min)(worst, R.agreeClamp); }
        std::ostringstream c;
        c.setf(std::ios::fixed);
        c.precision(2);
        c << " | CLAMPED Present count: " << misses << " of " << total << " frames wrong, worst segment " << 100.0 * worst << " %";
        verdict += c.str();
    }

    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    o << "Pairing test (Phase 4, Test A) " << stamp << "\n"
      << "render_mode=" << RenderModeName(GetRenderMode()) << "  seed=0x" << std::hex << g.seed << std::dec << "\n"
      << "verdict=" << verdict << "\n"
      << "N = game frames between our write of a pose and the Present of the frame showing it, anchored at the\n"
      << "    latest camera write when the frame was presented (0 = that very write).\n"
      << "random-flip frames: " << totalRandom << "\n";
    for (int sg = 0; sg < kSegmentCount; ++sg) {
        const SegResult& R = results[sg];
        o << "\n[segment " << sg + 1 << "] " << kSegments[sg].name << "\n";
        if (R.randomFrames == 0) { o << "  no data\n"; continue; }
        o << "  frames " << R.randomFrames << ", clearly one pose " << R.valid << ", ambiguous " << R.ambiguous
          << " (" << 100.0 * R.ambiguous / (std::max)(1, R.randomFrames) << " %)\n"
          << "  pose separation " << R.separation << " (unit-length feature vectors; 0 = poses look alike, 2 = opposite)\n"
          << "  held frames not clearly their pose: +60 " << R.holdPlusBad << ", -60 " << R.holdMinusBad << " (of " << R.holdCount << ")\n";
        if (R.valid == 0) continue;
        o << "  agreement by N:";
        for (int n = 0; n <= kMaxN; ++n) o << "  " << n << '=' << R.agree[n];
        // Each game frame's flip is an independent coin toss, so a wrong N still matches half the time: the share of
        // frames that really show the write from N frames earlier is 2 x agreement - 1.
        o << "\n  distribution of N (share of frames = 2 x agreement - 1; shares under 3 % not shown -- chance level is about +-2 % at 2500 frames):";
        for (int n = 0; n <= kMaxN; ++n) {
            const double share = (std::max)(0.0, 2.0 * R.agree[n] - 1.0);
            if (share >= 0.03) o << "  N=" << n << ": " << 100.0 * share << " %";
        }
        o << "\n  best N = " << R.bestN << " (" << 100.0 * R.agree[R.bestN] << " % of frames; " << R.mismatchBest << " frames disagree)\n"
          << "  per-frame N (from +-" << kWindow << " neighbours):";
        for (const auto& kv : R.windowN) o << "  " << (kv.first < 0 ? std::string("undetermined") : std::to_string(kv.first)) << '=' << kv.second;
        o << "\n  Present-count anchor (H = first G + Presents since): distribution of N:";
        for (int n = 0; n <= kMaxN; ++n) {
            const double share = (std::max)(0.0, 2.0 * R.agreeH[n] - 1.0);
            if (share >= 0.03) o << "  N=" << n << ": " << 100.0 * share << " %";
        }
        o << "  (best N " << R.bestNH << ", " << 100.0 * R.agreeH[R.bestNH] << " % agree)";
        o << "\n  CLAMPED Present count (S = clamp(previous S + 1, G-2, G-1)): " << 100.0 * R.agreeClamp << " % of frames paired right, "
          << R.clampMiss << " wrong; clamp moved S up " << R.clampUp << ", down " << R.clampDown << " times";
        o << "\n  written game frame between Presents: repeated " << R.gameFrameRepeats << ", skipped >=1 " << R.gameFrameSkips
          << "; mean Present interval " << R.presentMs << " ms\n"
          << "  " << (R.fixed ? "FIXED N" : "NOT FIXED") << "\n";
    }
    o << "\nper-frame data: vrmod_pairing_" << stamp << ".csv\n";
    o << "frame recorder CSVs (Test B, write persistence): the vrmod_frames_*/vrmod_camwrites_* written at the end of the test\n";

    const std::string txtName = "vrmod_pairing_" + stamp + ".txt";
    std::ofstream(txtName, std::ios::binary) << o.str();

    std::ostringstream c;
    c.setf(std::ios::fixed);
    c << "segment,phase,present,game_frame,t_ms,score,class,frame_n,sign_at_best_n";
    for (int p = 0; p < kFeat; ++p) c << ",lum" << p;
    c << "\n";
    for (size_t i = 0; i < g.recs.size(); ++i) {
        const Rec& r = g.recs[i];
        const SegResult& R = results[r.seg];
        c << (r.seg + 1) << ',' << kPhaseNames[r.phase] << ',' << r.present << ',' << r.gameFrame << ',';
        c.precision(3);
        c << r.tMs << ',';
        if (!std::isnan(score[i])) c << score[i];
        c << ',' << cls[i] << ',';
        if (frameN[i] >= -1) c << frameN[i];
        c << ',';
        if (R.bestN >= 0 && r.gameFrame >= static_cast<uint64_t>(R.bestN)) c << FrameLogRandomFlipSign(r.gameFrame - R.bestN);
        c.precision(2);
        for (int p = 0; p < kFeat; ++p) { c << ','; if (r.have) c << r.f[p]; }
        c << "\n";
    }
    std::ofstream("vrmod_pairing_" + stamp + ".csv", std::ios::binary) << c.str();

    modlog::Line("PAIRING: " + verdict + " -> " + txtName);
    for (int sg = 0; sg < kSegmentCount; ++sg) {
        const SegResult& R = results[sg];
        if (R.valid == 0) continue;
        std::ostringstream l;
        l.setf(std::ios::fixed);
        l.precision(1);
        l << "PAIRING:   segment " << sg + 1 << " (" << kSegments[sg].name << "): N distribution";
        for (int n = 0; n <= kMaxN; ++n) {
            const double share = (std::max)(0.0, 2.0 * R.agree[n] - 1.0);
            if (share >= 0.03) l << " N=" << n << ':' << 100.0 * share << '%';
        }
        l << "; Present-count anchor N " << R.bestNH << " (" << 100.0 * R.agreeH[R.bestNH] << " %); CLAMPED " << 100.0 * R.agreeClamp
          << " % (" << R.clampMiss << " wrong)";
        l << ", ambiguous " << 100.0 * R.ambiguous / (std::max)(1, R.randomFrames) << " %";
        modlog::Line(l.str());
    }
    PlayCue(!aborted && allFixed ? Cue::Success : Cue::Failure);
}

void Finish(bool aborted, const std::string& why) {
    FrameLogForceFlipMode(FlipMode::Off);
    HeadTrackingFreeze(false);
    CameraReasonEnd(ThirdPersonReason::Diagnostic);
    OpenXRSetPipelineConfig(g.saved);
    if (g.fence) HarvestCompleted(true);
    ReleaseResources();
    g.running = false;
    g.blackoutTail = 15;
    FrameLogStopRecording();
    if (aborted) modlog::Line("PAIRING: test aborted -- " + why);
    Analyse(aborted);
    g.recs.clear();
    g.recs.shrink_to_fit();
}

bool CtrlPPressed() {
    static bool wasDown = false;
    const bool down = (GetAsyncKeyState('P') & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
}

void EnterPhase(int phase) {
    g.phase = phase;
    g.inPhase = 0;
    FrameLogForceFlipMode(PhaseFlip(phase));
}

void EnterSegment(int seg) {
    g.seg = seg;
    XrPipelineConfig c;
    c.framesAhead = kSegments[seg].framesAhead;
    c.halfRate = kSegments[seg].halfRate;
    OpenXRSetPipelineConfig(c);
    modlog::Line(std::string("PAIRING: segment ") + std::to_string(seg + 1) + " of " + std::to_string(kSegmentCount)
        + ": " + kSegments[seg].name);
    EnterPhase(Settle);
}

} // namespace

bool PairingTestBlackout() {
    return g.running || g.blackoutTail > 0;
}

void PairingTestOnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) {
    if (!device || !queue || !backbuffer) return;
    const bool pressed = CtrlPPressed();

    if (!g.running) {
        if (g.blackoutTail > 0) --g.blackoutTail;
        if (!pressed) return;
        if (MenuScreenActive() || !IsHeadTrackingEnabled()) {
            modlog::Line("PAIRING: start it during gameplay with head tracking on (no menu, map or video).");
            PlayCue(Cue::Failure);
            return;
        }
        if (!Setup(device, backbuffer)) {
            PlayCue(Cue::Failure);
            return;
        }
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        g.seed = static_cast<uint64_t>(t.QuadPart) * 0x9E3779B97F4A7C15ull;
        FrameLogSetFlipSeed(g.seed);
        g.saved = OpenXRGetPipelineConfig();
        g.recs.clear();
        g.recs.reserve(static_cast<size_t>(kSegmentCount) * (kSettle + 2 * kHold + kRandomFrames) + 64);
        g.presents = 0;
        g.running = true;
        HeadTrackingFreeze(true);
        CameraReasonBegin(ThirdPersonReason::Diagnostic, 900.0);
        FrameLogStartRecording();
        modlog::Line("PAIRING: test started (~3-4 min; headset black, head tracking frozen, third-person camera; "
                     "keep the mouse and controller still). Ctrl+P cancels.");
        PlayCue(Cue::On);
        EnterSegment(0);
        return;
    }

    if (pressed) { Finish(true, "cancelled with Ctrl+P"); return; }
    if (MenuScreenActive() || !IsHeadTrackingEnabled()) { Finish(true, "a menu opened or head tracking went off"); return; }

    HarvestCompleted(false);
    ++g.presents;

    if (g.phase != Settle) {
        Rec r;
        r.seg = static_cast<uint8_t>(g.seg);
        r.phase = static_cast<uint8_t>(g.phase);
        r.present = FrameLogCurrentFrame();
        r.gameFrame = CameraLatestGameFrame();
        r.tMs = NowMs();
        g.recs.push_back(r);
        if (!QueueCopy(queue, backbuffer, static_cast<long long>(g.recs.size() - 1))) {
            Finish(true, "readback stalled");
            return;
        }
    }

    const int stallEvery = kSegments[g.seg].stallEvery;
    if (g.phase == Random && stallEvery > 0 && g.inPhase % stallEvery == stallEvery - 1) CameraInjectStall(kSegments[g.seg].stallMs);
    if (++g.inPhase < PhaseLength(g.phase)) return;
    if (g.phase < Random) { EnterPhase(g.phase + 1); return; }
    if (g.seg + 1 < kSegmentCount) { EnterSegment(g.seg + 1); return; }
    Finish(false, "");
}

#ifdef PAIRING_TEST_HARNESS
// Offline check of the analysis: synthetic frames that show the pose written `n` game frames earlier (with a
// fraction `jitter` of frames showing n+1 instead), one Present per game frame, with feature noise and exposure
// changes. Segment 4 uses n + 1 (to check per-segment reporting).
void PairingTestSynthetic(int n, double jitter, double noise, unsigned seed) {
    g.seed = 0x1234567ull + seed;
    FrameLogSetFlipSeed(g.seed);
    g.recs.clear();
    uint32_t s = seed * 2654435761u + 1;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0; };
    float plus[kFeat], minus[kFeat];
    for (int p = 0; p < kFeat; ++p) { plus[p] = static_cast<float>(p < 8 ? 190 + 20 * rnd() : 70 + 30 * rnd()); minus[p] = static_cast<float>(p < 8 ? 60 + 20 * rnd() : 140 + 30 * rnd()); }
    uint64_t gf = 1000;
    uint64_t present = 1;
    for (int sg = 0; sg < kSegmentCount; ++sg) {
        const int segN = sg == 3 ? n + 1 : n;
        for (int ph = HoldPlus; ph <= Random; ++ph) {
            for (int k = 0; k < PhaseLength(ph); ++k, ++gf, ++present) {
                int shown;
                if (ph == HoldPlus) shown = 1;
                else if (ph == HoldMinus) shown = -1;
                else { const int nn = segN + (rnd() < jitter ? 1 : 0); shown = FrameLogRandomFlipSign(gf - nn); }
                Rec r;
                r.seg = static_cast<uint8_t>(sg);
                r.phase = static_cast<uint8_t>(ph);
                r.present = present;
                r.gameFrame = gf;
                r.tMs = present * 13.9;
                const double gain = 0.8 + 0.4 * rnd(), off = 20.0 * (rnd() - 0.5);
                for (int p = 0; p < kFeat; ++p) r.f[p] = static_cast<float>(gain * (shown > 0 ? plus[p] : minus[p]) + off + noise * (rnd() - 0.5) * 3.46);
                r.have = true;
                g.recs.push_back(r);
            }
        }
    }
    Analyse(false);
}
#endif

} // namespace vrmod
