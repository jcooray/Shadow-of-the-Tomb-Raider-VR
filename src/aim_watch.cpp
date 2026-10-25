#include "aim_watch.h"

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aim_finder.h"
#include "audio_cue.h"
#include "camera_interceptor.h"
#include "logger.h"
#include "perf_stats.h"

namespace vrmod {

namespace {

constexpr int kWatches = 4;
constexpr int kMaxSites = 128;
constexpr int kStackQwords = 96;   // stack slots scanned for return addresses into SOTTR.exe
constexpr int kMaxRets = 12;
constexpr double kWatchMs = 1500.0;

// Per writing instruction. The thread that claims the slot fills the snapshot, then sets `ready`.
struct Site {
    std::atomic<uint64_t> rip{0};
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> perWatch[kWatches]{};
    std::atomic<uint32_t> tid{0};
    std::atomic<bool> ready{false};
    uint64_t gpr[16] = {};     // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    float xmm[6][4] = {};
    uint64_t rets[kMaxRets] = {};
    int nRets = 0;
    float value = 0.0f;        // the watched float after the write
    int watch = 0;
};
Site g_sites[kMaxSites];
std::atomic<uint64_t> g_dropped{0};
uintptr_t g_addr[kWatches] = {};
std::string g_label[kWatches];
int g_count = 0;
uint64_t g_modLo = 0, g_modHi = 0;
PVOID g_veh = nullptr;
bool g_running = false;
double g_startMs = 0.0;
int g_armed = 0, g_armFailed = 0, g_frames = 0;

void Snapshot(Site& s, const CONTEXT* c, int watch) {
    const DWORD64 r[16] = { c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi,
                            c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15 };
    for (int i = 0; i < 16; ++i) s.gpr[i] = r[i];
    const M128A* x = &c->Xmm0;
    for (int i = 0; i < 6; ++i) memcpy(s.xmm[i], &x[i], 16);
    s.watch = watch;
    memcpy(&s.value, reinterpret_cast<const void*>(g_addr[watch]), 4);
    // The stack is the faulting thread's own (a vectored handler runs on it), below its StackBase.
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uint64_t hi = reinterpret_cast<uint64_t>(tib->StackBase);
    const uint64_t* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
    for (int i = 0; i < kStackQwords && s.nRets < kMaxRets; ++i) {
        if (reinterpret_cast<uint64_t>(sp + i + 1) > hi) break;
        const uint64_t v = sp[i];
        if (v >= g_modLo && v < g_modHi) s.rets[s.nRets++] = v;
    }
}

void Record(const CONTEXT* c, int watch) {
    const uint64_t rip = c->Rip;
    size_t h = static_cast<size_t>((rip * 0x9E3779B97F4A7C15ull) >> 57) % kMaxSites;
    for (int probe = 0; probe < kMaxSites; ++probe, h = (h + 1) % kMaxSites) {
        Site& s = g_sites[h];
        uint64_t cur = s.rip.load(std::memory_order_acquire);
        bool mine = false;
        if (cur == 0) {
            uint64_t expected = 0;
            mine = s.rip.compare_exchange_strong(expected, rip);
            cur = mine ? rip : expected;
        }
        if (cur != rip) continue;
        if (mine) {
            s.tid.store(GetCurrentThreadId());
            Snapshot(s, c, watch);
            s.ready.store(true, std::memory_order_release);
        }
        s.count.fetch_add(1, std::memory_order_relaxed);
        s.perWatch[watch].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_dropped.fetch_add(1, std::memory_order_relaxed);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const DWORD64 dr6 = c->Dr6 & 0xF;
    if (dr6 == 0) return EXCEPTION_CONTINUE_SEARCH;
    const int watch = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : 3;
    if (watch < g_count) Record(c, watch);
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct ArmJob { uintptr_t addr[kWatches] = {}; int count = 0; int threads = 0, failed = 0; };

DWORD WINAPI ArmWorker(LPVOID p) {
    ArmJob* job = static_cast<ArmJob*>(p);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 1;
    // DR7: local enable Ln = bit 2n; R/W = 01 (write only) and LEN = 11 (4 bytes) at bits 16+4n -> nibble 0xD.
    DWORD64 enable = 0;
    for (int n = 0; n < job->count; ++n) enable |= (1ull << (2 * n)) | (0xDull << (16 + 4 * n));
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

void RunArmJob(ArmJob* job) {
    HANDLE t = CreateThread(nullptr, 0, ArmWorker, job, 0, nullptr);
    if (!t) return;
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
}

std::string Hex(uint64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// Ghidra addresses (image base 0x140000000) for SOTTR.exe code; module+offset otherwise.
std::string Where(uint64_t a) {
    if (a >= g_modLo && a < g_modHi) return Hex(0x140000000ull + (a - g_modLo));
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(a), &mod) && mod) {
        char path[MAX_PATH] = "?";
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* slash = strrchr(path, '\\');
        return std::string(slash ? slash + 1 : path) + "+" + Hex(a - reinterpret_cast<uint64_t>(mod));
    }
    return Hex(a);
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

std::string Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void Finish() {
    ArmJob clear;
    RunArmJob(&clear);
    Sleep(20);
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    g_running = false;

    std::vector<Site*> sites;
    for (Site& s : g_sites) if (s.rip.load() && s.ready.load()) sites.push_back(&s);
    std::sort(sites.begin(), sites.end(), [](Site* a, Site* b) { return a->count.load() > b->count.load(); });
    static const char* const kReg[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(4);
    o << "Aim write watch " << Stamp() << ": " << g_frames << " frames, armed on " << g_armed << " threads (" << g_armFailed
      << " failed)" << (g_dropped.load() ? ", SITE TABLE FULL" : "") << "\n"
      << "Addresses are Ghidra addresses (image base 0x140000000). A write watchpoint fires AFTER the writing instruction:\n"
      << "'after' is the next instruction; the writing one ends at the '|' in the code bytes.\n\nwatching:\n";
    for (int w = 0; w < g_count; ++w) o << "  [" << w << "] " << Hex(g_addr[w]) << "  " << g_label[w] << "\n";
    for (Site* s : sites) {
        const double f = (std::max)(1, g_frames);
        o << "\nafter " << Where(s->rip.load()) << "  writes " << s->count.load() << " (" << s->count.load() / f << " per frame)  by watch";
        for (int w = 0; w < g_count; ++w) o << " [" << w << "]=" << s->perWatch[w].load();
        o << "  thread " << s->tid.load() << "\n  code: " << CodeBytes(s->rip.load()) << "\n  first hit (watch [" << s->watch
          << "], value after " << s->value << "):\n   ";
        for (int i = 0; i < 16; ++i) {
            o << " " << kReg[i] << "=" << Hex(s->gpr[i]);
            if (i % 4 == 3) o << "\n   ";
        }
        for (int i = 0; i < 6; ++i) o << " xmm" << i << "=(" << s->xmm[i][0] << " " << s->xmm[i][1] << " " << s->xmm[i][2] << " " << s->xmm[i][3] << ")";
        o << "\n  SOTTR.exe return addresses on the stack (nearest first):";
        for (int i = 0; i < s->nRets; ++i) o << " " << Where(s->rets[i]);
        o << "\n";
    }
    if (sites.empty()) o << "\nno writes seen.\n";
    const std::string name = "vrmod_aimwatch_" + Stamp() + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();
    modlog::Line("AIMWATCH: " + std::to_string(sites.size()) + " writing instructions -> " + name);
    PlayCue(sites.empty() ? Cue::Failure : Cue::Success);
}

} // namespace

#ifdef AIM_WATCH_HARNESS
bool g_watchForce = false;
void AimWatchForceStart() { g_watchForce = true; }
bool AimWatchRunning() { return g_running; }
#endif

void AimWatchOnPresent() {
    static bool wWas = false;
    const bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    bool pressed = w && !wWas && ca;
    wWas = w;
#ifdef AIM_WATCH_HARNESS
    pressed = pressed || g_watchForce;
    g_watchForce = false;
#endif
    if (g_running) {
        ++g_frames;
        if (NowMs() - g_startMs >= kWatchMs) Finish();
        return;
    }
    if (!pressed) return;
    const std::vector<AimWatchTarget> t = AimFinderWatchTargets();
    if (t.empty() || MenuScreenActive()) {
        modlog::Line("AIMWATCH: run the aim finder (Ctrl+Alt+Y) first, in gameplay.");
        PlayCue(Cue::Failure);
        return;
    }
    std::vector<uintptr_t> a;
    std::vector<const char*> l;
    for (const AimWatchTarget& x : t) { a.push_back(x.addr); l.push_back(x.what.c_str()); }
    AimWatchStart(a.data(), l.data(), static_cast<int>(a.size()));
}

bool AimWatchStart(const uintptr_t* addrs, const char* const* labels, int count) {
    if (g_running || count <= 0) return false;
    const HMODULE exe = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
    g_modLo = reinterpret_cast<uint64_t>(exe);
    g_modHi = g_modLo + nt->OptionalHeader.SizeOfImage;
    for (Site& s : g_sites) {
        s.ready.store(false);
        s.rip.store(0);
        s.count.store(0);
        for (auto& p : s.perWatch) p.store(0);
        s.nRets = 0;
    }
    g_dropped.store(0);
    g_count = (std::min)(count, kWatches);
    ArmJob job;
    job.count = g_count;
    for (int i = 0; i < g_count; ++i) {
        g_addr[i] = addrs[i] & ~uintptr_t(3);
        g_label[i] = labels[i];
        job.addr[i] = g_addr[i];
    }
    g_veh = AddVectoredExceptionHandler(1, Handler);
    if (!g_veh) { modlog::Line("AIMWATCH: could not install the exception handler."); PlayCue(Cue::Failure); return false; }
    RunArmJob(&job);
    g_armed = job.threads;
    g_armFailed = job.failed;
    g_frames = 0;
    g_startMs = NowMs();
    g_running = true;
    modlog::Line("AIMWATCH: watching " + std::to_string(g_count) + " addresses for writes on " + std::to_string(g_armed) + " threads (1.5 s).");
    PlayCue(Cue::On);
    return true;
}

} // namespace vrmod
