#include "value_scan.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "audio_cue.h"
#include "camera_interceptor.h"
#include "head_tracking.h"
#include "logger.h"
#include "perf_stats.h"

namespace vrmod {

namespace {

constexpr int kSettlePresents = 30;     // tagging on -> scan (tags reach every copy)
constexpr int kSamplePresents = 120;    // per-Present reads of every hit after the scan
constexpr size_t kMaxHits = 200000;
constexpr size_t kMaxSampled = 4096;
constexpr int kTags = 64;

enum Kind : uint8_t { Raw = 0, Half, Cot, Tan, Deg, KindCount };
const char* const kKindNames[KindCount] = { "fov (raw)", "fov/2", "cot(fov/2) = projection [1][1]", "tan(fov/2)", "fov in degrees" };

// Candidate values for each tag: tagged FOV t -> derived float bits. Decoding picks the nearest within kUlpTol.
constexpr uint32_t kUlpTol = 2;
struct Table {
    uint32_t bits[KindCount][kTags];
    uint32_t lo[KindCount], hi[KindCount];   // accepted bit range per kind (positive floats: bits are monotonic)
    uint32_t rawBase;                         // FOV bits without the tag (low 8 bits clear)
};
Table g_t;

uint32_t Bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
float FromBits(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

void BuildTable(float fov) {
    g_t.rawBase = Bits(fov) & ~0xFFu;
    for (int t = 0; t < kTags; ++t) {
        const double f = FromBits(g_t.rawBase | (static_cast<uint32_t>(t) << 2));
        g_t.bits[Raw][t] = g_t.rawBase | (static_cast<uint32_t>(t) << 2);
        g_t.bits[Half][t] = Bits(static_cast<float>(f * 0.5));
        g_t.bits[Cot][t] = Bits(static_cast<float>(1.0 / std::tan(0.5 * f)));
        g_t.bits[Tan][t] = Bits(static_cast<float>(std::tan(0.5 * f)));
        g_t.bits[Deg][t] = Bits(static_cast<float>(f * 57.29577951308232));
    }
    for (int k = 0; k < KindCount; ++k) {
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (int t = 0; t < kTags; ++t) { lo = (std::min)(lo, g_t.bits[k][t]); hi = (std::max)(hi, g_t.bits[k][t]); }
        g_t.lo[k] = lo - kUlpTol;
        g_t.hi[k] = hi + kUlpTol;
    }
}

// Tag of a value of the given kind, or -1 if it doesn't decode cleanly.
int Decode(int kind, uint32_t v) {
    if (v < g_t.lo[kind] || v > g_t.hi[kind]) return -1;
    if (kind == Raw) return (v & 3) == 0 ? static_cast<int>((v >> 2) & 63) : -1;
    int best = -1;
    uint32_t bestD = 0xFFFFFFFFu;
    for (int t = 0; t < kTags; ++t) {
        const uint32_t c = g_t.bits[kind][t];
        const uint32_t d = v > c ? v - c : c - v;
        if (d < bestD) { bestD = d; best = t; }
    }
    return bestD <= kUlpTol ? best : -1;
}

struct Hit {
    uint64_t addr;
    uint8_t kind;
};
Hit* g_hits = nullptr;
size_t g_hitCount = 0;

// Plain C scan of one region (SEH-guarded by the caller). Records values whose bits fall in any kind's range.
void ScanWords(const uint32_t* p, size_t n) {
    const uint32_t rawLo = g_t.lo[Raw], rawHi = g_t.hi[Raw];
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v = p[i];
        int kind = -1;
        if (v >= rawLo && v <= rawHi) kind = Raw;
        else {
            for (int k = Half; k < KindCount; ++k) if (v >= g_t.lo[k] && v <= g_t.hi[k]) { kind = k; break; }
        }
        if (kind < 0) continue;
        if (Decode(kind, v) < 0) continue;
        if (g_hitCount < kMaxHits) g_hits[g_hitCount++] = { reinterpret_cast<uint64_t>(p + i), static_cast<uint8_t>(kind) };
    }
}

bool ScanRegionGuarded(const void* base, size_t size) {
    __try {
        ScanWords(static_cast<const uint32_t*>(base), size / 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadGuarded(uint64_t addr, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct RegionInfo {
    uint64_t base = 0, size = 0;
    DWORD type = 0, protect = 0;
};

std::string Hex(uint64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

std::string Describe(uint64_t addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return "?";
    std::string s;
    if (mbi.Type == MEM_IMAGE) {
        HMODULE mod = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
            GetModuleFileNameA(mod, path, MAX_PATH);
            const char* slash = strrchr(path, '\\');
            s = std::string(slash ? slash + 1 : path) + "+" + Hex(addr - reinterpret_cast<uint64_t>(mod));
        } else {
            s = "image";
        }
    } else {
        s = mbi.Type == MEM_MAPPED ? "mapped" : "private";
        s += " region " + Hex(reinterpret_cast<uint64_t>(mbi.AllocationBase)) + "+" + Hex(addr - reinterpret_cast<uint64_t>(mbi.AllocationBase));
    }
    if (mbi.Protect & PAGE_WRITECOMBINE) s += " [write-combined: likely a GPU upload heap]";
    if (mbi.Protect & PAGE_NOCACHE) s += " [uncached]";
    return s;
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

struct Sampled {
    uint64_t addr;
    uint8_t kind;
    int lagHist[kTags + 1] = {};   // [64] = didn't decode
    int changes = 0, samples = 0;
    uint32_t last = 0;
    bool haveLast = false;
};

struct State {
    int phase = 0;          // 0 idle, 1 settling, 2 sampling
    int count = 0;
    float fov = 0.0f;
    double scanMs = 0.0;
    uint64_t scannedBytes = 0;
    int regions = 0, faulted = 0;
    std::vector<Sampled> sampled;
    std::map<std::string, int> regionCounts;
    size_t hitsFound = 0;
};
State g;

// The scan is time-sliced (kSliceMs per Present) so the game keeps running. Write-combined and uncached regions are
// skipped: on this system they are GPU-visible mappings (upload heaps, Resizable-BAR VRAM) of several GB where CPU
// reads crawl at tens of MB/s -- the first in-game run scanned them in one go on the Present thread and froze the game
// for minutes. Their count and size are reported instead.
constexpr double kSliceMs = 8.0;
constexpr size_t kChunk = 4u << 20;   // bytes per SEH-guarded read before checking the clock

struct Region {
    uint8_t* base;
    size_t size;
};

struct ScanState {
    std::vector<Region> regions;
    size_t cursor = 0, offset = 0;
    uint64_t totalBytes = 0, skippedWcBytes = 0;
    int skippedWcRegions = 0;
    double startMs = 0.0, lastProgressMs = 0.0;
};
ScanState g_scan;

void BuildRegions() {
    if (!g_hits) g_hits = static_cast<Hit*>(VirtualAlloc(nullptr, sizeof(Hit) * kMaxHits, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_hitCount = 0;
    // Our own module is skipped (its code and the tables above would match).
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&BuildRegions), &self);
    g_scan = ScanState{};
    g.scannedBytes = 0;
    g.regions = 0;
    g.faulted = 0;
    uint8_t* p = reinterpret_cast<uint8_t*>(0x10000);
    MEMORY_BASIC_INFORMATION mbi{};
    while (reinterpret_cast<uint64_t>(p) < 0x7FFFFFFF0000ull && VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* next = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY
            || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
        const bool ours = (self && mbi.AllocationBase == self) || mbi.AllocationBase == g_hits;
        if (mbi.State == MEM_COMMIT && readable && !(mbi.Protect & PAGE_GUARD) && !ours) {
            if (mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) {
                ++g_scan.skippedWcRegions;
                g_scan.skippedWcBytes += mbi.RegionSize;
            } else {
                g_scan.regions.push_back({ static_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize });
                g_scan.totalBytes += mbi.RegionSize;
            }
        }
        if (next <= p) break;
        p = next;
    }
    g.regions = static_cast<int>(g_scan.regions.size());
    g_scan.startMs = g_scan.lastProgressMs = NowMs();
}

// Scans for up to kSliceMs; returns true when every region has been scanned.
bool ScanSlice() {
    const double t0 = NowMs();
    while (g_scan.cursor < g_scan.regions.size()) {
        const Region& r = g_scan.regions[g_scan.cursor];
        const size_t n = (std::min)(kChunk, r.size - g_scan.offset);
        if (ScanRegionGuarded(r.base + g_scan.offset, n)) {
            g.scannedBytes += n;
            g_scan.offset += n;
        } else {
            ++g.faulted;   // freed or protected since it was listed: skip the rest of this region
            g_scan.offset = r.size;
        }
        if (g_scan.offset >= r.size) { ++g_scan.cursor; g_scan.offset = 0; }
        if (NowMs() - t0 >= kSliceMs) break;
    }
    if (NowMs() - g_scan.lastProgressMs > 2000.0) {
        g_scan.lastProgressMs = NowMs();
        modlog::Line("VALUESCAN: " + std::to_string(g.scannedBytes >> 20) + " / " + std::to_string(g_scan.totalBytes >> 20) + " MB scanned, "
            + std::to_string(g_hitCount) + " hits so far.");
    }
    return g_scan.cursor >= g_scan.regions.size();
}

void FinishScan() {
    g.scanMs = NowMs() - g_scan.startMs;
    g.hitsFound = g_hitCount;
    // Sample list: all hits up to kMaxSampled (raw kinds first, as they are the most certain).
    std::vector<Hit> hits(g_hits, g_hits + g_hitCount);
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.kind < b.kind; });
    g.sampled.clear();
    for (size_t i = 0; i < hits.size() && g.sampled.size() < kMaxSampled; ++i) {
        Sampled s;
        s.addr = hits[i].addr;
        s.kind = hits[i].kind;
        g.sampled.push_back(s);
    }
}

void SampleOnce() {
    const uint64_t latest = CameraLatestGameFrame();
    for (Sampled& s : g.sampled) {
        uint32_t v;
        if (!ReadGuarded(s.addr, &v)) continue;
        ++s.samples;
        if (s.haveLast && v != s.last) ++s.changes;
        s.last = v;
        s.haveLast = true;
        const int tag = Decode(s.kind, v);
        if (tag < 0) { ++s.lagHist[kTags]; continue; }
        ++s.lagHist[static_cast<int>((latest - static_cast<uint64_t>(tag)) & 63)];
    }
}

void Report() {
    const std::string stamp = TimeStamp();
    std::vector<Sampled*> order;
    for (Sampled& s : g.sampled) order.push_back(&s);
    std::stable_sort(order.begin(), order.end(), [](const Sampled* a, const Sampled* b) { return a->changes > b->changes; });

    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    o << "Value scan (FOV pose tag) " << stamp << "\n"
      << "fov " << g.fov << " rad (bits " << Hex(g_t.rawBase) << " + tag<<2); scanned " << g.scannedBytes / (1024 * 1024) << " MB in "
      << g.regions << " regions (" << g.faulted << " faulted) in " << g.scanMs << " ms; " << g.hitsFound << " hits"
      << (g.hitsFound >= kMaxHits ? " (LIMIT REACHED)" : "") << "; " << g.sampled.size() << " sampled at " << kSamplePresents << " Presents\n"
      << "not scanned: " << g_scan.skippedWcRegions << " write-combined/uncached regions, " << (g_scan.skippedWcBytes >> 20)
      << " MB (GPU-visible mappings; CPU reads are too slow)\n"
      << "lag = (latest camera write's game frame - the value's tag) mod 64, read at Present. 'changes' = samples where the\n"
      << "value differed from the previous Present. A copy that tracks the frame being presented would change every\n"
      << "Present and have a lag like the pairing test's N (2, sometimes 1 under load).\n\n";
    std::map<std::string, int> perKind;
    for (const Sampled& s : g.sampled) ++perKind[kKindNames[s.kind]];
    o << "hits by kind:";
    for (const auto& kv : perKind) o << "  " << kv.first << ": " << kv.second;
    o << "\n\n";
    int listed = 0;
    for (const Sampled* s : order) {
        if (listed >= 300) break;
        ++listed;
        o << Hex(s->addr) << "  " << kKindNames[s->kind] << "  " << Describe(s->addr) << "\n  changes " << s->changes << "/" << (s->samples > 0 ? s->samples - 1 : 0)
          << "; lag:";
        std::vector<std::pair<int, int>> lags;
        for (int l = 0; l < kTags; ++l) if (s->lagHist[l]) lags.push_back({ s->lagHist[l], l });
        std::sort(lags.rbegin(), lags.rend());
        for (size_t i = 0; i < lags.size() && i < 6; ++i) o << "  " << lags[i].second << " (" << 100.0 * lags[i].first / (std::max)(1, s->samples) << " %)";
        if (s->lagHist[kTags]) o << "  undecodable " << s->lagHist[kTags];
        o << "\n";
    }
    if (static_cast<int>(order.size()) > listed) o << "... " << order.size() - listed << " more (fewer changes)\n";
    const std::string name = "vrmod_valuescan_" + stamp + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();

    int live = 0;
    for (const Sampled& s : g.sampled) if (s.changes >= kSamplePresents / 2) ++live;
    modlog::Line("VALUESCAN: " + std::to_string(g.hitsFound) + " hits (" + std::to_string(g.scannedBytes / (1024 * 1024)) + " MB scanned in "
        + std::to_string(static_cast<int>(g.scanMs)) + " ms), " + std::to_string(live) + " change at least every other Present -> " + name);
}

bool CtrlTPressed() {
    static bool wasDown = false;
    const bool down = (GetAsyncKeyState('T') & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
}

} // namespace

#ifdef VALUE_SCAN_HARNESS
bool g_forceStart = false;
void ValueScanForceStart() { g_forceStart = true; }
#endif

void ValueScanOnPresent() {
    bool pressed = CtrlTPressed();
#ifdef VALUE_SCAN_HARNESS
    pressed = pressed || g_forceStart;
    g_forceStart = false;
#endif
    if (g.phase == 0) {
        if (!pressed) return;
        if (!CameraStructAddress() || MenuScreenActive() || !IsHeadTrackingEnabled()) {
            modlog::Line("VALUESCAN: start it during gameplay with head tracking on.");
            PlayCue(Cue::Failure);
            return;
        }
        g.fov = HeadTrackingGetGameFov();
        BuildTable(g.fov);
        CameraSetFovTagging(true);
        g.phase = 1;
        g.count = 0;
        modlog::Line("VALUESCAN: FOV tagging on; scanning in " + std::to_string(kSettlePresents) + " frames (the game will pause for a few seconds).");
        PlayCue(Cue::On);
        return;
    }
    if (g.phase == 1) {
        if (++g.count < kSettlePresents) return;
        BuildRegions();
        modlog::Line("VALUESCAN: scanning " + std::to_string(g_scan.totalBytes >> 20) + " MB in " + std::to_string(g_scan.regions.size())
            + " regions, " + std::to_string(static_cast<int>(kSliceMs)) + " ms per frame; skipping " + std::to_string(g_scan.skippedWcRegions)
            + " write-combined/uncached regions (" + std::to_string(g_scan.skippedWcBytes >> 20) + " MB).");
        g.phase = 3;
        return;
    }
    if (g.phase == 3) {
        const bool done = ScanSlice();
        const bool timedOut = !done && NowMs() - g_scan.startMs > 120000.0;   // hard cap: never run for more than 2 minutes
        if (!done && !timedOut) return;
        if (timedOut) modlog::Line("VALUESCAN: scan stopped after 2 minutes (" + std::to_string(g.scannedBytes >> 20) + " MB done); using what was found.");
        FinishScan();
        modlog::Line("VALUESCAN: scan done (" + std::to_string(g.hitsFound) + " hits, " + std::to_string(g.scannedBytes >> 20) + " MB in "
            + std::to_string(static_cast<int>(g.scanMs)) + " ms); sampling.");
        g.phase = 2;
        g.count = 0;
        return;
    }
    SampleOnce();
    if (++g.count < kSamplePresents) return;
    CameraSetFovTagging(false);
    Report();
    g.phase = 0;
    PlayCue(Cue::Success);
}

} // namespace vrmod
