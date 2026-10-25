#include "cambone_probe.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sstream>

#include "logger.h"
#include "present_hook.h"

namespace vrmod {

namespace {

// FUN_1408c8710, image base 0x140000000. Verified live: this is the only place the game touches a joint-index
// field at object offset 0x1A8, resolves a per-joint world transform from a global table, and writes the result
// into the SAME camera struct layout our own camera_interceptor.cpp reads/writes (+0x80 pos, +0xa0 quat, +0xb0
// fov) -- so it's a generic "aim a Camera object at a skeleton joint" updater, whatever the C++ class name.
constexpr uintptr_t kUpdateRva = 0x8C8710;
constexpr unsigned char kUpdatePrologue[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18,
    0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x38, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB0, 0x01
};
constexpr uintptr_t kJointIndexOffset = 0x1A8;   // *(uint32_t*)(thisPtr + 0x1A8): the joint/slot index
constexpr uintptr_t kCameraPtrOffset = 0x160;    // *(void**)(thisPtr + 0x160): the Camera struct this writes into

using Update_t = void(*)(void* thisPtr, float dt, void* param3);
Update_t g_origUpdate = nullptr;

double NowMs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

void LogIfDue(void* thisPtr) {
    static std::atomic<uint32_t> lastJoint{0xFFFFFFFFu};
    static std::atomic<double> lastLogMs{0.0};

    const uint32_t joint = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(thisPtr) + kJointIndexOffset);
    const double now = NowMs();
    const bool changed = joint != lastJoint.load();
    if (!changed && now - lastLogMs.load() < 500.0) {
        return;
    }
    lastJoint.store(joint);
    lastLogMs.store(now);

    void* cam = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(thisPtr) + kCameraPtrOffset);
    std::ostringstream o;
    o << "CAMBONE joint=" << joint << " thisPtr=0x" << std::hex << reinterpret_cast<uintptr_t>(thisPtr);
    if (cam) {
        const float* p = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(cam) + 0x80);
        const float* q = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(cam) + 0xA0);
        const float fov = *reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(cam) + 0xB0);
        o << std::dec << " pos=(" << p[0] << "," << p[1] << "," << p[2] << ")"
          << " quat=(" << q[0] << "," << q[1] << "," << q[2] << "," << q[3] << ")"
          << " fov=" << fov << " camPtr=0x" << std::hex << reinterpret_cast<uintptr_t>(cam);
    } else {
        o << " (no camera pointer yet)";
    }
    modlog::Line(o.str());
}

void hkUpdate(void* thisPtr, float dt, void* param3) {
    g_origUpdate(thisPtr, dt, param3);   // real update first: camPtr/pose may only be valid after this runs
    __try {
        LogIfDue(thisPtr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Our guess at the object layout could be wrong for some caller; never take the game down over a probe.
    }
}

} // namespace

bool InstallCameraBoneProbe() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        return false;
    }
    void* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(exe) + kUpdateRva);
    if (memcmp(target, kUpdatePrologue, sizeof(kUpdatePrologue)) != 0) {
        modlog::Line("CameraBoneProbe: prologue mismatch at the expected RVA (different build?); probe off.");
        return false;
    }
    if (!EnsureMinHookInitialized()) {
        return false;
    }
    if (MH_CreateHook(target, &hkUpdate, reinterpret_cast<void**>(&g_origUpdate)) != MH_OK
        || MH_EnableHook(target) != MH_OK) {
        modlog::Line("CameraBoneProbe: hook install failed.");
        return false;
    }
    modlog::Line("CameraBoneProbe: installed. Logs CAMBONE lines (joint index + resulting pose) whenever a "
        "bone-attached camera ticks -- trigger a dialogue/cutscene close-up to see what index a head shot uses.");
    return true;
}

}
