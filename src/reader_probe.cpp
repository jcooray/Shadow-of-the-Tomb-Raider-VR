#include "reader_probe.h"

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
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

// Stage 1 watches the camera struct itself; each later stage watches where the previous stage saw it copied to
// (up to two destinations, rotation and FOV of each), following the chain towards the render thread.
constexpr int kMaxStages = 4;
constexpr double kStageSeconds[kMaxStages] = { 1.5, 1.0, 1.0, 1.0 };
constexpr int kWatches = 4;              // debug registers DR0-DR3
constexpr int kMaxSites = 512;           // distinct instructions per stage
constexpr int kMaxCands = 64;            // copy destinations seen per stage
constexpr size_t kMaxHits = 262144;      // individual accesses kept with their time (all stages)
constexpr uintptr_t kCopyWindow = 0xC0;  // a register in [base, base + this] is taken as a copy source (the camera fields end at 0xB4)

struct Watch {
    uintptr_t addr = 0;       // watched 4 bytes
    uintptr_t structBase = 0; // the struct it belongs to (for copy detection)
    std::string label;
};

// Per instruction: counts per watch and up to 8 threads. Filled by the exception handler (lock-free).
struct Site {
    std::atomic<uint64_t> rip{0};        // 0 = free slot
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> perWatch[kWatches]{};
    std::atomic<uint32_t> tids[8]{};
};
Site g_sites[kMaxSites];

// A copy destination: a struct base inferred from source/destination registers at an access.
struct Cand {
    std::atomic<uint64_t> base{0};
    std::atomic<uint32_t> count{0};
    std::atomic<uint64_t> rip{0};
    std::atomic<uint32_t> tid{0};
    std::atomic<uint32_t> fromWatch{0};
};
Cand g_cands[kMaxCands];

struct Hit {
    double tMs;
    uint64_t rip;
    uint32_t tid;
    uint8_t stage, watch;
};
Hit* g_hits = nullptr;
std::atomic<size_t> g_hitCount{0};
std::atomic<uint64_t> g_droppedSites{0};
std::atomic<int> g_stage{0};
uintptr_t g_watchBase[kWatches] = {};   // read by the handler; only changed while disarmed
PVOID g_veh = nullptr;

void RecordSite(uint64_t rip, uint32_t tid, int watch) {
    size_t h = static_cast<size_t>((rip * 0x9E3779B97F4A7C15ull) >> 55) % kMaxSites;
    for (int probe = 0; probe < kMaxSites; ++probe, h = (h + 1) % kMaxSites) {
        Site& s = g_sites[h];
        uint64_t cur = s.rip.load(std::memory_order_acquire);
        if (cur == 0) {
            uint64_t expected = 0;
            cur = s.rip.compare_exchange_strong(expected, rip) ? rip : expected;
        }
        if (cur != rip) continue;
        s.count.fetch_add(1, std::memory_order_relaxed);
        s.perWatch[watch].fetch_add(1, std::memory_order_relaxed);
        for (auto& t : s.tids) {
            uint32_t v = t.load(std::memory_order_relaxed);
            if (v == tid) break;
            if (v == 0) {
                uint32_t zero = 0;
                if (t.compare_exchange_strong(zero, tid) || zero == tid) break;
            }
        }
        return;
    }
    g_droppedSites.fetch_add(1, std::memory_order_relaxed);
}

void RecordCand(uint64_t base, uint64_t rip, uint32_t tid, int watch) {
    size_t h = static_cast<size_t>((base * 0x9E3779B97F4A7C15ull) >> 58) % kMaxCands;
    for (int probe = 0; probe < kMaxCands; ++probe, h = (h + 1) % kMaxCands) {
        Cand& c = g_cands[h];
        uint64_t cur = c.base.load(std::memory_order_acquire);
        if (cur == 0) {
            uint64_t expected = 0;
            if (c.base.compare_exchange_strong(expected, base)) {
                c.rip.store(rip);
                c.tid.store(tid);
                c.fromWatch.store(static_cast<uint32_t>(watch));
                cur = base;
            } else {
                cur = expected;
            }
        }
        if (cur != base) continue;
        c.count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

// A copy: a source register pointing into the watched struct, and a destination register. The destination struct's
// base is the destination register shifted by the same offset (both point at the same field).
// Copies into the copying thread's own stack are temporaries (the first in-game run followed one at SOTTR+0x161175
// into stack slots reused by thousands of unrelated accesses), so they are skipped. The stack range is the TEB's
// DeallocationStack (x64 TEB +0x1478, the bottom of the reserved stack) up to NT_TIB.StackBase.
bool OnOwnStack(uint64_t addr) {
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uint64_t hi = reinterpret_cast<uint64_t>(tib->StackBase);
    const uint64_t lo = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const char*>(NtCurrentTeb()) + 0x1478);
    return addr >= lo && addr < hi;
}

void CheckCopy(const CONTEXT* c, int watch, uint64_t rip, uint32_t tid) {
    const uintptr_t base = g_watchBase[watch];
    const DWORD64 pairs[2][2] = { { c->Rdx, c->Rcx }, { c->Rsi, c->Rdi } };
    for (const auto& p : pairs) {
        const DWORD64 src = p[0], dst = p[1];
        if (src < base || src > base + kCopyWindow) continue;
        if (dst < 0x10000 || dst == src) continue;
        const uint64_t destBase = dst + (base - src);
        if (destBase + kCopyWindow > base && destBase < base + kCopyWindow) continue;   // overlaps the source struct
        if (OnOwnStack(destBase)) continue;
        RecordCand(destBase, rip, tid, watch);
    }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const DWORD64 dr6 = c->Dr6 & 0xF;
    if (dr6 == 0) return EXCEPTION_CONTINUE_SEARCH;   // not one of our watchpoints
    const int watch = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : 3;
    const uint32_t tid = GetCurrentThreadId();
    RecordSite(c->Rip, tid, watch);
    CheckCopy(c, watch, c->Rip, tid);
    if (g_hits) {
        const size_t i = g_hitCount.fetch_add(1, std::memory_order_relaxed);
        if (i < kMaxHits) g_hits[i] = { NowMs(), c->Rip, tid, static_cast<uint8_t>(g_stage.load(std::memory_order_relaxed)), static_cast<uint8_t>(watch) };
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets (or clears, with no addresses) the watchpoints on every thread of the process except the calling one. Runs on
// a worker thread so the Present thread (which may itself be a reader) gets them too.
struct ArmJob {
    uintptr_t addr[kWatches] = {};
    int count = 0;
    int threads = 0, failed = 0;
};

DWORD WINAPI ArmWorker(LPVOID p) {
    ArmJob* job = static_cast<ArmJob*>(p);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 1;
    // DR7: local enable Ln = bit 2n; for each, R/W = 11 (read or write) and LEN = 11 (4 bytes) at bits 16+4n.
    DWORD64 enable = 0;
    for (int n = 0; n < job->count; ++n) enable |= (1ull << (2 * n)) | (0xFull << (16 + 4 * n));
    const DWORD64 ourMask = 0x55ull | (0xFFFFull << 16);
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) { ++job->failed; continue; }
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                ctx.Dr0 = job->count > 0 ? job->addr[0] : 0;
                ctx.Dr1 = job->count > 1 ? job->addr[1] : 0;
                ctx.Dr2 = job->count > 2 ? job->addr[2] : 0;
                ctx.Dr3 = job->count > 3 ? job->addr[3] : 0;
                ctx.Dr7 = (ctx.Dr7 & ~ourMask) | enable;
                ctx.Dr6 = 0;
                if (SetThreadContext(th, &ctx)) ++job->threads; else ++job->failed;
            } else {
                ++job->failed;
            }
            ResumeThread(th);
        } else {
            ++job->failed;
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    return 0;
}

bool RunArmJob(ArmJob* job) {
    HANDLE t = CreateThread(nullptr, 0, ArmWorker, job, 0, nullptr);
    if (!t) return false;
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    return true;
}

std::string Hex(uint64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

std::string ModuleOffset(uint64_t addr) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
        char path[MAX_PATH] = "?";
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* slash = strrchr(path, '\\');
        return std::string(slash ? slash + 1 : path) + "+" + Hex(addr - reinterpret_cast<uint64_t>(mod));
    }
    return Hex(addr);
}

std::string CodeBytes(uint64_t rip) {
    unsigned char b[40];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(rip - 32), b, sizeof(b), &got) || got != sizeof(b)) return "(unreadable)";
    std::string s;
    char h[4];
    for (int i = 0; i < 40; ++i) {
        if (i == 32) s += "| ";
        snprintf(h, sizeof(h), "%02X ", b[i]);
        s += h;
    }
    return s;
}

std::string ThreadName(uint32_t tid) {
    using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static GetDesc fn = reinterpret_cast<GetDesc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    if (!fn) return "";
    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!th) return "";
    PWSTR desc = nullptr;
    std::string out;
    if (SUCCEEDED(fn(th, &desc)) && desc) {
        char buf[256];
        WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, sizeof(buf), nullptr, nullptr);
        out = buf;
        LocalFree(desc);
    }
    CloseHandle(th);
    return out;
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

struct SiteRow { uint64_t rip; uint32_t count, w[kWatches]; std::vector<uint32_t> tids; };
struct CandRow { uint64_t base; uint32_t count; uint64_t rip; uint32_t tid, fromWatch; };
struct StageResult {
    std::vector<Watch> watches;
    std::vector<SiteRow> sites;
    std::vector<CandRow> cands;
    int armedThreads = 0, armFailed = 0;
    double startMs = 0.0, endMs = 0.0;
};

struct State {
    bool running = false;
    int stage = 0;
    double stageStart = 0.0;
    uintptr_t base = 0;
    uint32_t presentTid = 0, writeTid = 0;
    std::vector<double> presents;
    std::vector<StageResult> stages;
    std::set<uint64_t> watched;   // struct bases already watched
    bool startedRecorder = false;
};
State g;

void ResetTables() {
    for (Site& s : g_sites) {
        s.rip.store(0);
        s.count.store(0);
        for (auto& w : s.perWatch) w.store(0);
        for (auto& t : s.tids) t.store(0);
    }
    for (Cand& c : g_cands) {
        c.base.store(0);
        c.count.store(0);
        c.rip.store(0);
        c.tid.store(0);
        c.fromWatch.store(0);
    }
}

void Collect(StageResult* r) {
    for (Site& s : g_sites) {
        const uint64_t rip = s.rip.load();
        if (!rip) continue;
        SiteRow row{ rip, s.count.load(), {}, {} };
        for (int w = 0; w < kWatches; ++w) row.w[w] = s.perWatch[w].load();
        for (auto& t : s.tids) if (t.load()) row.tids.push_back(t.load());
        r->sites.push_back(row);
    }
    std::sort(r->sites.begin(), r->sites.end(), [](const SiteRow& a, const SiteRow& b) { return a.count > b.count; });
    for (Cand& c : g_cands) {
        const uint64_t b = c.base.load();
        if (!b) continue;
        r->cands.push_back({ b, c.count.load(), c.rip.load(), c.tid.load(), c.fromWatch.load() });
    }
    std::sort(r->cands.begin(), r->cands.end(), [](const CandRow& a, const CandRow& b) { return a.count > b.count; });
}

// Arms the given watches for a new stage (disarming the previous ones in the same pass).
bool ArmStage(const std::vector<Watch>& watches) {
    ArmJob job;
    job.count = static_cast<int>(watches.size());
    for (int i = 0; i < job.count; ++i) {
        job.addr[i] = watches[i].addr;
        g_watchBase[i] = watches[i].structBase;
    }
    ResetTables();
    StageResult r;
    r.watches = watches;
    const bool ok = RunArmJob(&job) && job.threads > 0;
    r.armedThreads = job.threads;
    r.armFailed = job.failed;
    r.startMs = NowMs();
    g.stages.push_back(r);
    g_stage.store(static_cast<int>(g.stages.size()));
    for (const Watch& w : watches) g.watched.insert(w.structBase);
    return ok;
}

// Next stage's watches: the two most frequent copy destinations not yet watched (rotation and FOV of each).
std::vector<Watch> NextWatches(const StageResult& r) {
    std::vector<Watch> out;
    int n = 0;
    for (const CandRow& c : r.cands) {
        if (g.watched.count(c.base) || c.count < 3 || (c.base & 3) != 0) continue;
        bool overlaps = false;   // skip destinations overlapping a struct already watched (or already chosen)
        for (uint64_t w : g.watched) if (c.base + kCopyWindow > w && c.base < w + kCopyWindow) overlaps = true;
        for (const Watch& o2 : out) if (c.base + kCopyWindow > o2.structBase && c.base < o2.structBase + kCopyWindow) overlaps = true;
        if (overlaps) continue;
        ++n;
        const std::string name = "copy " + std::to_string(g.stages.size()) + "." + std::to_string(n) + " at " + Hex(c.base);
        out.push_back({ c.base + 0xA0, c.base, name + " +0xA0 (rotation)" });
        out.push_back({ c.base + 0xB0, c.base, name + " +0xB0 (fov)" });
        if (n == 2) break;
    }
    return out;
}

void Disarm() {
    ArmJob job;   // no addresses: clears ours
    RunArmJob(&job);
    Sleep(20);   // let any exception already in flight finish in the handler
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    g_stage.store(0);
    g.running = false;
}

void Report() {
    const std::string stamp = TimeStamp();
    const size_t hits = (std::min)(g_hitCount.load(), kMaxHits);
    auto label = [&](uint32_t tid) {
        std::string tags;
        if (tid == g.writeTid) tags += "camera-write thread";
        if (tid == g.presentTid) tags += std::string(tags.empty() ? "" : ", ") + "Present thread";
        const std::string name = ThreadName(tid);
        if (!name.empty()) tags += std::string(tags.empty() ? "" : ", ") + "\"" + name + "\"";
        return tags.empty() ? std::to_string(tid) : std::to_string(tid) + " (" + tags + ")";
    };
    // Phase of each hit relative to the preceding Present, per (stage, rip).
    std::map<std::pair<int, uint64_t>, std::vector<double>> phase;
    for (size_t i = 0; i < hits; ++i) {
        const Hit& h = g_hits[i];
        auto it = std::upper_bound(g.presents.begin(), g.presents.end(), h.tMs);
        if (it == g.presents.begin()) continue;
        phase[{ h.stage, h.rip }].push_back(h.tMs - *(it - 1));
    }

    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "Camera-struct reader probe (copy-following) " << stamp << "\n"
      << "camera struct " << Hex(g.base) << "; camera-write thread " << label(g.writeTid) << "; Present thread " << label(g.presentTid) << "\n"
      << g.presents.size() << " Presents; " << g_hitCount.load() << " accesses" << (g_droppedSites.load() ? "; SITE TABLE FULL in some stage" : "") << "\n"
      << "A data watchpoint fires AFTER the accessing instruction: 'after' is the NEXT instruction; the accessing one ends at\n"
      << "the '|' in the code bytes. A 'copy' is an access where a source register (rdx or rsi) points into the watched\n"
      << "struct: its destination (rcx or rdi, same offset) is watched in the next stage.\n";
    for (size_t s = 0; s < g.stages.size(); ++s) {
        const StageResult& r = g.stages[s];
        const double frames = (std::max)(1.0, static_cast<double>(std::count_if(g.presents.begin(), g.presents.end(),
            [&](double t) { return t >= r.startMs && t < r.endMs; })));
        o << "\n==================== stage " << s + 1 << " (" << frames << " frames, armed on " << r.armedThreads << " threads, "
          << r.armFailed << " failed)\nwatching:";
        for (size_t w = 0; w < r.watches.size(); ++w) o << "\n  [" << w << "] " << Hex(r.watches[w].addr) << "  " << r.watches[w].label;
        o << "\n\n";
        for (const SiteRow& row : r.sites) {
            o << "after " << ModuleOffset(row.rip) << "  accesses " << row.count << " (" << row.count / frames << " per frame)  by watch";
            for (size_t w = 0; w < r.watches.size(); ++w) o << " [" << w << "]=" << row.w[w];
            o << "\n  threads:";
            for (uint32_t t : row.tids) o << "  " << label(t);
            auto it = phase.find({ static_cast<int>(s + 1), row.rip });
            if (it != phase.end() && !it->second.empty()) {
                std::vector<double> v = it->second;
                std::sort(v.begin(), v.end());
                o << "\n  ms after the preceding Present: min " << v.front() << ", median " << v[v.size() / 2] << ", max " << v.back();
            }
            o << "\n  code: " << CodeBytes(row.rip) << "\n";
        }
        o << "\ncopy destinations seen in this stage (struct base, times, by, thread):\n";
        if (r.cands.empty()) o << "  none\n";
        for (const CandRow& c : r.cands) {
            o << "  " << Hex(c.base) << "  x" << c.count << " (" << c.count / frames << " per frame)  by after " << ModuleOffset(c.rip)
              << " from watch [" << c.fromWatch << "]  thread " << label(c.tid) << (g.watched.count(c.base) ? "  (watched)" : "") << "\n";
        }
    }
    const std::string txtName = "vrmod_readers_" + stamp + ".txt";
    std::ofstream(txtName, std::ios::binary) << o.str();

    std::ostringstream c;
    c.setf(std::ios::fixed);
    c.precision(3);
    c << "kind,t_ms,stage,after_instruction,tid,watch\n";
    for (double t : g.presents) c << "present," << t << ",,,,\n";
    std::map<uint64_t, std::string> names;
    for (size_t i = 0; i < hits; ++i) {
        const Hit& h = g_hits[i];
        auto it = names.find(h.rip);
        if (it == names.end()) it = names.emplace(h.rip, ModuleOffset(h.rip)).first;
        c << "access," << h.tMs << ',' << static_cast<int>(h.stage) << ',' << it->second << ',' << h.tid << ',' << static_cast<int>(h.watch) << "\n";
    }
    std::ofstream("vrmod_readers_" + stamp + ".csv", std::ios::binary) << c.str();

    modlog::Line("READERS: " + std::to_string(g.stages.size()) + " stages, " + std::to_string(g_hitCount.load()) + " accesses -> " + txtName);
    for (size_t s = 0; s < g.stages.size(); ++s) {
        const StageResult& r = g.stages[s];
        std::ostringstream l;
        l << "READERS:   stage " << s + 1 << ": " << r.sites.size() << " instructions;";
        std::set<uint32_t> others;
        for (const SiteRow& row : r.sites) for (uint32_t t : row.tids) if (t != g.writeTid) others.insert(t);
        l << " threads other than the camera-write thread:";
        for (uint32_t t : others) l << ' ' << t << (t == g.presentTid ? "(present)" : "");
        modlog::Line(l.str());
    }
}

bool CtrlRPressed() {
    static bool wasDown = false;
    const bool down = (GetAsyncKeyState('R') & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
}

} // namespace

#ifdef READER_PROBE_HARNESS
bool g_forceStart = false;
void ReaderProbeForceStart() { g_forceStart = true; }
#endif

void ReaderProbeOnPresent() {
    bool pressed = CtrlRPressed();
#ifdef READER_PROBE_HARNESS
    pressed = pressed || g_forceStart;
    g_forceStart = false;
#endif
    if (g.running) {
        g.presents.push_back(NowMs());
        if (NowMs() - g.stageStart < kStageSeconds[g.stage] * 1000.0) return;
        StageResult& cur = g.stages.back();
        cur.endMs = NowMs();
        Collect(&cur);
        std::vector<Watch> next = (g.stage + 1 < kMaxStages) ? NextWatches(cur) : std::vector<Watch>{};
        if (!next.empty() && ArmStage(next)) {
            ++g.stage;
            g.stageStart = NowMs();
            modlog::Line("READERS: stage " + std::to_string(g.stage + 1) + ": following " + std::to_string(next.size() / 2) + " copy destination(s).");
            return;
        }
        Disarm();
        if (g.startedRecorder) FrameLogStopRecording();
        Report();
        PlayCue(Cue::Success);
        return;
    }
    if (!pressed) return;
    g.base = CameraStructAddress();
    if (!g.base || MenuScreenActive()) {
        modlog::Line("READERS: start it during gameplay (camera struct not known yet, or a menu is open).");
        PlayCue(Cue::Failure);
        return;
    }
    if (!g_hits) g_hits = new Hit[kMaxHits];
    g_hitCount.store(0);
    g_droppedSites.store(0);
    g.presents.clear();
    g.presents.reserve(2048);
    g.stages.clear();
    g.watched.clear();
    g.stage = 0;
    g.presentTid = GetCurrentThreadId();
    g.writeTid = CameraWriteThreadId();
    g.startedRecorder = !FrameLogIsRecording();
    if (g.startedRecorder) FrameLogStartRecording();
    g_veh = AddVectoredExceptionHandler(1, Handler);
    if (!g_veh) {
        modlog::Line("READERS: could not install the exception handler.");
        PlayCue(Cue::Failure);
        return;
    }
    const std::vector<Watch> first = {
        { g.base + 0x80, g.base, "camera struct +0x80 (position)" },
        { g.base + 0xA0, g.base, "camera struct +0xA0 (rotation)" },
        { g.base + 0xB0, g.base, "camera struct +0xB0 (fov)" },
    };
    if (!ArmStage(first)) {
        modlog::Line("READERS: could not arm any thread.");
        Disarm();
        PlayCue(Cue::Failure);
        return;
    }
    g.running = true;
    g.stageStart = NowMs();
    modlog::Line("READERS: stage 1: watching the camera struct at " + Hex(g.base) + " on " + std::to_string(g.stages.back().armedThreads)
        + " threads (up to " + std::to_string(kMaxStages) + " stages, following copies).");
    PlayCue(Cue::On);
}

} // namespace vrmod
