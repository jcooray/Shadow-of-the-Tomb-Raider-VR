#include "ansel_camera_hook.h"

#include <windows.h>
#include <MinHook.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <cstdint>

#include "logger.h"

namespace vrmod {

namespace {

// Verified via Ghidra decompilation of the real game exe, not guessed:
// the function at this RVA contains the literal call
// `ansel::updateCamera((Camera *)(param_1 + 0xc))`.
constexpr uintptr_t kAnselWrapperRVA = 0x100f850;
constexpr uintptr_t kCameraStructOffset = 0xc;

// How many floats to dump starting at param_1+0xc. NVIDIA's public Ansel SDK
// Camera struct is roughly: Vec3 position (12 bytes) + Quaternion rotation
// (16 bytes) + float fov + float projectionOffsetX + float projectionOffsetY
// (40 bytes total, 10 floats) -- dumping 16 floats (64 bytes) leaves margin
// in case the real layout differs slightly from what we recall.
constexpr int kFloatsToLog = 16;

using AnselWrapper_t = void(__stdcall*)(uintptr_t, uintptr_t, char);
AnselWrapper_t oAnselWrapper = nullptr;

std::atomic<unsigned long long> g_callCount{0};

void __stdcall hkAnselWrapper(uintptr_t param_1, uintptr_t param_2, char param_3) {
    oAnselWrapper(param_1, param_2, param_3);

    auto count = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1) {
        modlog::Line("AnselCameraHook: first call, param_1=0x"
            + [&]{ std::ostringstream o; o << std::hex << param_1; return o.str(); }());
    }

    if (GetAsyncKeyState(VK_F9) & 0x8000) {
        const float* floats = reinterpret_cast<const float*>(param_1 + kCameraStructOffset);
        std::ostringstream oss;
        oss << "ANSELCAM param_1=0x" << std::hex << param_1 << std::dec << " floats=[";
        oss << std::fixed << std::setprecision(6);
        for (int i = 0; i < kFloatsToLog; ++i) {
            if (i > 0) {
                oss << ((i % 4 == 0) ? " | " : " ");
            }
            oss << floats[i];
        }
        oss << "]";
        modlog::Line(oss.str());
    }
}

} // namespace

bool InstallAnselCameraHook() {
    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!moduleBase) {
        modlog::Line("AnselCameraHook: GetModuleHandleW failed.");
        return false;
    }

    void* target = reinterpret_cast<void*>(moduleBase + kAnselWrapperRVA);

    // MH_Initialize() is already called by InstallPresentHook() before this runs.
    if (MH_CreateHook(target, &hkAnselWrapper, reinterpret_cast<void**>(&oAnselWrapper)) != MH_OK
        || MH_EnableHook(target) != MH_OK) {
        modlog::Line("AnselCameraHook: hook setup failed.");
        return false;
    }

    modlog::Line("AnselCameraHook: installed at RVA 0x100f850. Press F9 in-game to dump the Camera struct.");
    return true;
}

} // namespace vrmod
