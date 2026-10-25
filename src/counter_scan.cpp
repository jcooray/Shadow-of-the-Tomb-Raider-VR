#include "counter_scan.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "audio_cue.h"
#include "camera_interceptor.h"
#include "frame_log.h"
#include "logger.h"
#include "perf_stats.h"

namespace vrmod {

namespace {

constexpr int kSnapshots = 4;          // consecutive Presents; a candidate rises by exactly 1 between each
constexpr int kSamplePresents = 300;
constexpr int kStallEvery = 25;        // Presents between injected game-thread stalls
constexpr int kStallMs = 20;
constexpr int kAfterStall = 4;         // Presents after an injected stall counted as "after stall"
constexpr size_t kMaxCandidates = 4096;

struct Candidate {
    uint32_t rva;                       // offset from the module base
    uint32_t last = 0;
    bool haveLast = false;
    std::map<int, int> delta;           // value change per Present
    std::map<long long, int> offNormal, offAfterStall;   // value - G
};

struct State {
    int phase = 0;                      // 0 idle, 1 snapshots, 2 sampling
    int count = 0;
    uint8_t* module = nullptr;
    uint32_t dataRva = 0, dataSize = 0;
    std::vector<uint32_t> prev;         // previous snapshot (words)
    std::vector<uint32_t> cand;         // candidate word indices
    std::vector<Candidate> cands;
    std::map<long long, int> presentNormal, presentAfterStall;   // our Present counter - G, for reference
    int stallsInjected = 0;
    int sinceStall = 1000;
};
State g;

bool FindData() {
    g.module = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g.module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g.module + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".data", 6) == 0) {
            g.dataRva = sec[i].VirtualAddress;
            g.dataSize = sec[i].Misc.VirtualSize & ~3u;
            return true;
        }
    }
    return false;
}

void Snapshot(std::vector<uint32_t>* out) {
    out->resize(g.dataSize / 4);
    memcpy(out->data(), g.module + g.dataRva, g.dataSize);
}

std::string Hex(uint64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string Hist(const std::map<long long, int>& h, int total) {
    std::vector<std::pair<int, long long>> v;
    for (const auto& kv : h) v.push_back({ kv.second, kv.first });
    std::sort(v.rbegin(), v.rend());
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    for (size_t i = 0; i < v.size() && i < 4; ++i) o << "  " << v[i].second << " (" << 100.0 * v[i].first / (std::max)(1, total) << " %)";
    if (v.size() > 4) o << "  +" << v.size() - 4 << " more";
    return o.str();
}

void Report() {
    const std::string stamp = TimeStamp();
    // Interesting first: counters whose offset to G differs after stalls (render/present side), then the rest.
    struct Row { const Candidate* c; int normalTotal, stallTotal; long long normalMode, stallMode; double oneStep; bool shifts; };
    std::vector<Row> rows;
    for (const Candidate& c : g.cands) {
        Row r{ &c, 0, 0, 0, 0, 0.0, false };
        int best = -1;
        for (const auto& kv : c.offNormal) { r.normalTotal += kv.second; if (kv.second > best) { best = kv.second; r.normalMode = kv.first; } }
        best = -1;
        for (const auto& kv : c.offAfterStall) { r.stallTotal += kv.second; if (kv.second > best) { best = kv.second; r.stallMode = kv.first; } }
        int steps = 0, ones = 0;
        for (const auto& kv : c.delta) { steps += kv.second; if (kv.first == 1) ones = kv.second; }
        r.oneStep = steps ? static_cast<double>(ones) / steps : 0.0;
        r.shifts = r.stallTotal > 0 && r.normalTotal > 0 && (r.stallMode != r.normalMode || c.offAfterStall.size() > 1);
        rows.push_back(r);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.shifts != b.shifts) return a.shifts;
        return a.oneStep > b.oneStep;
    });

    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    o << "Counter scan (SOTTR.exe .data) " << stamp << "\n"
      << ".data at SOTTR.exe+" << Hex(g.dataRva) << ", " << g.dataSize / (1024 * 1024) << " MB; " << g.cands.size()
      << " words rose by exactly 1 on " << kSnapshots - 1 << " consecutive Presents"
      << (g.cands.size() >= kMaxCandidates ? " (LIMIT)" : "") << "\n"
      << kSamplePresents << " Presents sampled; " << g.stallsInjected << " game-thread stalls of " << kStallMs << " ms injected (every "
      << kStallEvery << " Presents); 'after stall' = the " << kAfterStall << " Presents following one.\n"
      << "G = latest camera write's game frame at Present. offset = value - G.\n"
      << "Reference, our own Present counter - G: normal" << Hist(g.presentNormal, kSamplePresents) << " | after stall"
      << Hist(g.presentAfterStall, kSamplePresents) << "\n\n"
      << "A counter following the game thread keeps one offset. One following the frame being rendered/presented changes\n"
      << "offset after stalls (the presented frame falls from 2 to 1 behind G) -- those are listed first.\n\n";
    for (const Row& r : rows) {
        const Candidate& c = *r.c;
        std::map<long long, int> d;
        for (const auto& kv : c.delta) d[kv.first] = kv.second;
        o << "SOTTR.exe+" << Hex(c.rva) << (r.shifts ? "  ** offset changes after stalls **" : "") << "\n  step per Present:" << Hist(d, kSamplePresents)
          << "\n  offset normal:" << Hist(c.offNormal, r.normalTotal) << "\n  offset after stall:" << Hist(c.offAfterStall, r.stallTotal) << "\n";
    }
    const std::string name = "vrmod_counters_" + stamp + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();
    int shifting = 0;
    for (const Row& r : rows) if (r.shifts) ++shifting;
    modlog::Line("COUNTERS: " + std::to_string(g.cands.size()) + " frame-rate counters in .data, " + std::to_string(shifting)
        + " change offset after injected stalls -> " + name);
}

bool CtrlNPressed() {
    static bool wasDown = false;
    const bool down = (GetAsyncKeyState('N') & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
}

} // namespace

void CounterScanOnPresent() {
    const bool pressed = CtrlNPressed();
    if (g.phase == 0) {
        if (!pressed) return;
        if (!CameraStructAddress() || MenuScreenActive() || !IsHeadTrackingEnabled() || !FindData()) {
            modlog::Line("COUNTERS: start it during gameplay with head tracking on.");
            PlayCue(Cue::Failure);
            return;
        }
        Snapshot(&g.prev);
        g.cand.clear();
        g.cands.clear();
        g.presentNormal.clear();
        g.presentAfterStall.clear();
        g.stallsInjected = 0;
        g.sinceStall = 1000;
        g.phase = 1;
        g.count = 1;
        modlog::Line("COUNTERS: snapshotting .data (" + std::to_string(g.dataSize >> 20) + " MB) on " + std::to_string(kSnapshots) + " Presents.");
        PlayCue(Cue::On);
        return;
    }
    if (g.phase == 1) {
        std::vector<uint32_t> cur;
        Snapshot(&cur);
        if (g.count == 1) {
            for (size_t i = 0; i < cur.size(); ++i) if (cur[i] == g.prev[i] + 1u) g.cand.push_back(static_cast<uint32_t>(i));
        } else {
            std::vector<uint32_t> keep;
            for (uint32_t i : g.cand) if (cur[i] == g.prev[i] + 1u) keep.push_back(i);
            g.cand.swap(keep);
        }
        g.prev.swap(cur);
        if (++g.count < kSnapshots) return;
        for (size_t k = 0; k < g.cand.size() && k < kMaxCandidates; ++k) {
            Candidate c;
            c.rva = g.dataRva + g.cand[k] * 4;
            g.cands.push_back(c);
        }
        g.prev.clear();
        g.prev.shrink_to_fit();
        modlog::Line("COUNTERS: " + std::to_string(g.cands.size()) + " candidates; sampling " + std::to_string(kSamplePresents)
            + " Presents with a " + std::to_string(kStallMs) + " ms game-thread stall every " + std::to_string(kStallEvery) + ".");
        g.phase = 2;
        g.count = 0;
        return;
    }
    // Sampling.
    const long long G = static_cast<long long>(CameraLatestGameFrame());
    const bool afterStall = g.sinceStall < kAfterStall;
    const long long presentOff = static_cast<long long>(FrameLogCurrentFrame()) - G;
    ++(afterStall ? g.presentAfterStall : g.presentNormal)[presentOff];
    for (Candidate& c : g.cands) {
        const uint32_t v = *reinterpret_cast<const volatile uint32_t*>(g.module + c.rva);
        if (c.haveLast) {
            const long long d = static_cast<long long>(v) - static_cast<long long>(c.last);
            ++c.delta[static_cast<int>((std::max)(-9ll, (std::min)(9ll, d)))];
        }
        c.last = v;
        c.haveLast = true;
        ++(afterStall ? c.offAfterStall : c.offNormal)[static_cast<long long>(v) - G];
    }
    ++g.sinceStall;
    if (++g.count % kStallEvery == 0) {
        CameraInjectStall(kStallMs);
        ++g.stallsInjected;
        g.sinceStall = 0;
    }
    if (g.count < kSamplePresents) return;
    Report();
    g.cands.clear();
    g.phase = 0;
    PlayCue(Cue::Success);
}

} // namespace vrmod
