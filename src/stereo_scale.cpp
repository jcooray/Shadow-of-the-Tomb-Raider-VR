#include "build_config.h"
#include "stereo_scale.h"

#include <windows.h>
#include <cmath>
#include <sstream>
#include <vector>

#include "logger.h"
#include "audio_cue.h"

namespace vrmod {

namespace {

struct StereoTriple { float separation, popout, nearMul; };

std::vector<uintptr_t> g_candidates;
bool g_scanned = false;

bool Near(float a, float b) { return std::fabs(a - b) < 2e-4f; }

bool ReadTriple(uintptr_t addr, StereoTriple* out) {
    __try {
        const volatile float* p = reinterpret_cast<const volatile float*>(addr);
        out->separation = p[0]; out->popout = p[1]; out->nearMul = p[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WriteFloat(uintptr_t addr, float v) {
    __try { *reinterpret_cast<volatile float*>(addr) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Looks for the (separation, popout, near multiplier) triple with the values the game logged at startup.
void Scan(float sep, float pop, float nearMul) {
    g_candidates.clear();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const bool rw = mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE) && mbi.RegionSize <= (512ull << 20);
        if (rw) {
            const float* p = reinterpret_cast<const float*>(base);
            const size_t n = mbi.RegionSize / sizeof(float);
            __try {
                for (size_t i = 0; i + 3 <= n; ++i) {
                    if (Near(p[i], sep) && Near(p[i + 1], pop) && Near(p[i + 2], nearMul)) {
                        g_candidates.push_back(reinterpret_cast<uintptr_t>(&p[i]));
                        if (g_candidates.size() >= 16) return;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        addr = base + mbi.RegionSize;
    }
}

void Report(const char* why) {
    std::ostringstream o;
    o << "StereoScale: " << why << " | " << g_candidates.size() << " copies:";
    for (uintptr_t a : g_candidates) {
        StereoTriple t{};
        if (ReadTriple(a, &t)) o << " [0x" << std::hex << a << std::dec << " sep=" << t.separation << " pop=" << t.popout << " near=" << t.nearMul << "]";
    }
    modlog::Line(o.str());
}

void Adjust(bool separation, float factor) {
    int changed = 0;
    for (uintptr_t a : g_candidates) {
        StereoTriple t{};
        if (!ReadTriple(a, &t)) continue;
        if (!(t.separation > 0.0f && t.separation < 2.0f && t.nearMul > 0.0f && t.nearMul < 20.0f)) continue;   // stale copy
        const uintptr_t field = a + (separation ? 0 : 8);
        const float cur = separation ? t.separation : t.nearMul;
        if (WriteFloat(field, cur * factor)) ++changed;
    }
    Report(separation ? (factor > 1 ? "separation up" : "separation down") : (factor > 1 ? "near multiplier up" : "near multiplier down"));
    if (changed == 0) modlog::Line("StereoScale: no live copy could be written (open the stereo settings once so the game reloads them, then retry).");
}

} // namespace

void StereoScalePollHotkeys() {
    static bool wasDown = false;
    const bool down = (VRMOD_DEV_KEY(VK_INSERT) & 0x8000) != 0;
    const bool justPressed = down && !wasDown;
    wasDown = down;
    if (!justPressed) return;

    if (!g_scanned) {
        g_scanned = true;
        Scan(0.022f, 0.271f, 0.35f);
        Report("scan done");
        if (g_candidates.empty()) {
            // The values may have been changed in the menu; the log line above lists nothing in that case.
            modlog::Line("StereoScale: default triple (0.022, 0.271, 0.35) not found -- reset the stereo sliders to defaults and retry.");
            g_scanned = false;
            return;
        }
    }
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    Adjust(!alt, ctrl ? 1.0f / 1.05f : 1.05f);
    PlayCue(Cue::On);
}

}
