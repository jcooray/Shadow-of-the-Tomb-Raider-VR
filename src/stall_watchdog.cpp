#include "stall_watchdog.h"

#include <windows.h>
#include <atomic>
#include <string>

#include "build_config.h"
#include "logger.h"

namespace vrmod {

namespace {

constexpr ULONGLONG kStallThresholdMs = 3000;
constexpr int kMaxReports = 30;

std::atomic<ULONGLONG> g_lastPresentTick{0};
std::atomic<const char*> g_phase{"none"};

DWORD WINAPI WatchdogThread(LPVOID) {
    int reports = 0;
    ULONGLONG lastReport = 0;
    for (;;) {
        Sleep(1000);
        ULONGLONG last = g_lastPresentTick.load(std::memory_order_relaxed);
        if (last == 0) {
            continue;
        }
        ULONGLONG now = GetTickCount64();
        if (now - last > kStallThresholdMs && now - lastReport > kStallThresholdMs && reports < kMaxReports) {
            lastReport = now;
            ++reports;
            modlog::Line("StallWatchdog: no Present for " + std::to_string(now - last)
                + " ms, last phase='" + g_phase.load(std::memory_order_relaxed) + "'");
        }
    }
    return 0;
}

} // namespace

void StartStallWatchdog() {
    HANDLE h = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
    }
}

#if VRMOD_DIAG
// Development build: how long each phase of the frame took, from one phase marker to the next (the Present thread sets
// them all). A phase over 4 ms is logged as SLOWPHASE with its name -- except the waits that are long by design (the
// runtime's xrWaitFrame, the driver's Present) and the game's own time between Presents, logged only beyond 16 ms.
// 2026-10-28: a stall of ~10 ms in the mod's own Present work every 1.5 s, in stretches, missed headset frames.
namespace {
LARGE_INTEGER g_phaseStart{}, g_qpf{};
uint64_t g_presents = 0;
int g_slowReports = 0;
// This Present's phases (the mod's own work, waits left out), for SLOWFRAME: over 5 ms in all -- normally ~1.2 ms -- the
// breakdown is logged, telling one slow step from everything slowed at once (the thread held up from outside).
struct PhaseTime { const char* name; double ms; };
PhaseTime g_frame[24];
int g_frameN = 0;
double g_frameWork = 0.0;
int g_slowFrames = 0;
}
#endif

void StallWatchdogNotePresent() {
    g_lastPresentTick.store(GetTickCount64(), std::memory_order_relaxed);
#if VRMOD_DIAG
    ++g_presents;
    if (g_frameWork > 5.0 && g_slowFrames < 300) {
        ++g_slowFrames;
        std::string line = "SLOWFRAME: Present " + std::to_string(g_presents - 1) + ", mod work " + std::to_string(static_cast<int>(g_frameWork * 10.0 + 0.5) / 10.0).substr(0, 5) + " ms:";
        for (int i = 0; i < g_frameN; ++i) {
            char b[64];
            snprintf(b, sizeof(b), " %s %.1f", g_frame[i].name, g_frame[i].ms);
            line += b;
        }
        modlog::Line(line);
    }
    g_frameN = 0;
    g_frameWork = 0.0;
#endif
}

void StallWatchdogSetPhase(const char* phase) {
#if VRMOD_DIAG
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    const char* prev = g_phase.load(std::memory_order_relaxed);
    if (g_phaseStart.QuadPart && g_slowReports < 400) {
        const double ms = static_cast<double>(now.QuadPart - g_phaseStart.QuadPart) * 1000.0 / static_cast<double>(g_qpf.QuadPart);
        const std::string p = prev;
        const bool waits = p == "xr:xrWaitFrame" || p == "hkPresent:oPresent" || p == "xr:xrWaitSwapchainImage";
        const double limit = p == "game:between-presents" ? 16.0 : 4.0;
        if (!waits && p != "game:between-presents") {
            g_frameWork += ms;
            if (g_frameN < 24) g_frame[g_frameN++] = { prev, ms };
        }
        if (!waits && ms > limit) {
            ++g_slowReports;
            char b[160];
            snprintf(b, sizeof(b), "SLOWPHASE: '%s' took %.1f ms (Present %llu)", prev, ms, static_cast<unsigned long long>(g_presents));
            modlog::Line(b);
        }
    }
    g_phaseStart = now;
#endif
    g_phase.store(phase, std::memory_order_relaxed);
}

} // namespace vrmod
