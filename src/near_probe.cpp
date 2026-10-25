#include "near_probe.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "audio_cue.h"
#include "camera_interceptor.h"
#include "logger.h"

namespace vrmod {

namespace {

// Finding the renderer's near clip distance (docs/FIRSTPERSON.md, step 5).
// - Writing the camera struct's culling plane (+0x100 = (0, 0, -1, 5): near at 5 cm) had no visible effect, so the
//   renderer builds its projection from a near value stored elsewhere.
// - FUN_1409cc3a0 (step 4) turned out to be a per-area detail/LOD scale (the user saw pop-in change), not the near.
// The camera manager keeps one 0xD50-byte slot per camera; our struct sits 0x1E0 into its slot's per-camera block
// (FUN_1409d6f30: puVar17 = manager + 0xD0 + slot * 0xD50, camera write at puVar17 + 0x78 floats). The near value is
// likely in that block or near it, stored as 5 (cm).
// Ctrl+Alt+Z dumps [struct - 0x800, struct + 0x1000) to vrmod_camslot_<time>.txt and lists every float equal to 5.0;
// Ctrl+Alt+X steps through those candidates, writing 30.0 into one at a time (on every camera write and every Present).
constexpr intptr_t kFrom = -0x800, kTo = 0x1000;
std::vector<intptr_t> g_cands;             // offsets from the struct holding 5.0
std::atomic<int> g_active{ -1 };           // index into g_cands being overridden (-1 = none)
std::atomic<intptr_t> g_activeOff{ 0 };
constexpr float kTestValue = 30.0f;

bool ReadGuarded(uintptr_t addr, void* out, size_t n) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void WriteGuarded(uintptr_t addr, float v) {
    __try {
        *reinterpret_cast<volatile float*>(addr) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::string Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

void Dump() {
    const uintptr_t base = CameraStructAddress();
    if (!base) { modlog::Line("NEARPROBE: camera struct unknown."); PlayCue(Cue::Failure); return; }
    std::ostringstream o;
    o << "Camera slot dump " << Stamp() << ": camera struct 0x" << std::hex << base << std::dec
      << "; offsets relative to the struct. Floats shown where plausible (|v| < 1e7), else the raw hex.\n";
    g_cands.clear();
    int lines = 0;
    for (intptr_t off = kFrom; off < kTo; off += 16) {
        uint32_t w[4];
        if (!ReadGuarded(base + off, w, 16)) { o << (off < 0 ? "-" : "+") << std::hex << (off < 0 ? -off : off) << std::dec << ": unreadable\n"; continue; }
        char line[200];
        int n = snprintf(line, sizeof(line), "%s0x%04llX:", off < 0 ? "-" : "+", static_cast<unsigned long long>(off < 0 ? -off : off));
        for (int k = 0; k < 4; ++k) {
            float f;
            memcpy(&f, &w[k], 4);
            if (w[k] == 0x40A00000u) g_cands.push_back(off + k * 4);   // exactly 5.0f
            if (f == f && f > -1e7f && f < 1e7f && (w[k] == 0 || (w[k] & 0x7F800000u) != 0)) n += snprintf(line + n, sizeof(line) - n, " %12.4f", f);
            else n += snprintf(line + n, sizeof(line) - n, "   0x%08X", w[k]);
        }
        o << line << "\n";
        ++lines;
    }
    o << "\nfloats equal to 5.0 (near-plane candidates), in Ctrl+Alt+X order:";
    for (size_t i = 0; i < g_cands.size(); ++i) {
        const intptr_t c = g_cands[i];
        o << "\n  " << i + 1 << ": " << (c < 0 ? "-0x" : "+0x") << std::hex << (c < 0 ? -c : c) << std::dec;
    }
    o << "\n";
    const std::string name = "vrmod_camslot_" + Stamp() + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();
    g_active.store(-1);
    modlog::Line("NEARPROBE: camera slot dumped (" + std::to_string(lines) + " lines) -> " + name + "; " + std::to_string(g_cands.size())
        + " floats equal to 5.0. Ctrl+Alt+X writes 30 into them one at a time.");
    PlayCue(g_cands.empty() ? Cue::Failure : Cue::Success);
}

bool Chord(int vk, bool& was) {
    const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool p = d && !was;
    was = d;
    return p && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
}

} // namespace

bool InstallNearProbe() {
    modlog::Line("NEARPROBE: Ctrl+Alt+Z dumps the camera slot and lists floats equal to 5.0; Ctrl+Alt+X overrides them one at a time with 30.");
    return true;
}

void NearProbeApply() {
    const int a = g_active.load(std::memory_order_relaxed);
    if (a < 0) return;
    const uintptr_t base = CameraStructAddress();
    if (base) WriteGuarded(base + g_activeOff.load(std::memory_order_relaxed), kTestValue);
}

void NearProbeOnPresent() {
    static bool zWas = false, xWas = false;
    if (Chord('Z', zWas)) Dump();
    if (Chord('X', xWas)) {
        if (g_cands.empty()) {
            modlog::Line("NEARPROBE: dump first (Ctrl+Alt+Z).");
            PlayCue(Cue::Failure);
        } else {
            // Restore the previous candidate to 5 before moving on.
            const int prev = g_active.load();
            const uintptr_t base = CameraStructAddress();
            if (prev >= 0 && base) WriteGuarded(base + g_activeOff.load(), 5.0f);
            int next = prev + 1;
            if (next >= static_cast<int>(g_cands.size())) next = -1;
            if (next >= 0) g_activeOff.store(g_cands[next]);
            g_active.store(next);
            if (next < 0) {
                modlog::Line("NEARPROBE: override OFF (all candidates restored to 5).");
                PlayCue(Cue::Off);
            } else {
                const intptr_t c = g_cands[next];
                char b[64];
                snprintf(b, sizeof(b), "%s0x%llX", c < 0 ? "-" : "+", static_cast<unsigned long long>(c < 0 ? -c : c));
                modlog::Line("NEARPROBE: candidate " + std::to_string(next + 1) + "/" + std::to_string(g_cands.size()) + " at struct " + b
                    + " = 30 (was 5).");
                PlayCue(Cue::On);
            }
        }
    }
    NearProbeApply();
}

}
