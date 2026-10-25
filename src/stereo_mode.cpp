#include "stereo_mode.h"

#include <windows.h>
#include <cstdint>
#include <atomic>
#include <sstream>
#include <iomanip>

#include "logger.h"

namespace vrmod {

namespace {

// See stereo_mode.h for provenance. The last offset (0x924) is added but not
// dereferenced -- it's the address of the int itself, not another pointer.
constexpr uintptr_t kModuleOffset = 0x36905B0;
constexpr uintptr_t kOffsetChain[] = { 0x90, 0x68, 0x68, 0x30, 0x38, 0xB0 };
constexpr uintptr_t kFinalOffset = 0x924;
constexpr int32_t kStereoValue3DVision = 2;

std::atomic<bool> g_stereoOverrideEnabled{false};
std::atomic<bool> g_haveOriginalValue{false};
std::atomic<int32_t> g_originalStereoValue{0};

// Reading/writing an address built from an offset chain that may be stale is
// exactly the kind of operation that can hit unmapped memory -- SEH lets us
// fail gracefully (log it) instead of crashing the whole game.
bool SafeReadPtr(uintptr_t address, uintptr_t* outValue) {
    __try {
        *outValue = *reinterpret_cast<uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadInt32(uintptr_t address, int32_t* outValue) {
    __try {
        *outValue = *reinterpret_cast<int32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteInt32(uintptr_t address, int32_t value) {
    __try {
        *reinterpret_cast<int32_t*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ResolveStereoModeAddress(uintptr_t* outAddress) {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        return false;
    }

    uintptr_t ptr = 0;
    if (!SafeReadPtr(base + kModuleOffset, &ptr) || !ptr) {
        return false;
    }

    for (uintptr_t offset : kOffsetChain) {
        uintptr_t next = 0;
        if (!SafeReadPtr(ptr + offset, &next) || !next) {
            return false;
        }
        ptr = next;
    }

    *outAddress = ptr + kFinalOffset;
    return true;
}

// Logs every hop of the chain (not just success/failure) so a stale chain
// shows exactly where it diverges -- the same debuggability approach used
// for the camera struct AOB scan earlier in this project.
void LogStereoModeProbe() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        modlog::Line("StereoModeProbe: no module base.");
        return;
    }

    uintptr_t ptr = 0;
    if (!SafeReadPtr(base + kModuleOffset, &ptr) || !ptr) {
        modlog::Line("StereoModeProbe: FAILED at base hop (moduleBase+0x36905B0 unreadable or null).");
        return;
    }

    std::ostringstream oss;
    oss << "StereoModeProbe: hop0=0x" << std::hex << ptr;

    int hopIndex = 1;
    for (uintptr_t offset : kOffsetChain) {
        uintptr_t next = 0;
        if (!SafeReadPtr(ptr + offset, &next) || !next) {
            oss << " FAILED at hop" << std::dec << hopIndex << " (+0x" << std::hex << offset << ")";
            modlog::Line(oss.str());
            return;
        }
        ptr = next;
        oss << " hop" << std::dec << hopIndex << "=0x" << std::hex << ptr;
        ++hopIndex;
    }

    uintptr_t finalAddr = ptr + kFinalOffset;
    int32_t value = 0;
    if (!SafeReadInt32(finalAddr, &value)) {
        oss << " FAILED reading final int at 0x" << std::hex << finalAddr;
        modlog::Line(oss.str());
        return;
    }

    oss << " finalAddr=0x" << std::hex << finalAddr << std::dec << " value=" << value
        << " (0=off, 1=side-by-side, 2=3D Vision)";
    modlog::Line(oss.str());
}

} // namespace

bool InstallStereoModeProbe() {
    modlog::Line("StereoModeProbe: ready. F6 to probe/log the stereo-mode pointer chain, "
                  "F7 to toggle forcing 3D Vision mode (value=2).");
    return true;
}

void PollStereoModeHotkeys() {
    // F12 collides with Steam's default screenshot hotkey -- F6 avoids it.
    static bool f6WasDown = false;
    bool f6IsDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (f6IsDown && !f6WasDown) {
        LogStereoModeProbe();
    }
    f6WasDown = f6IsDown;

    static bool f7WasDown = false;
    bool f7IsDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    bool f7JustPressed = f7IsDown && !f7WasDown;
    f7WasDown = f7IsDown;

    if (f7JustPressed) {
        bool enabling = !g_stereoOverrideEnabled.load(std::memory_order_relaxed);
        g_stereoOverrideEnabled.store(enabling, std::memory_order_relaxed);

        uintptr_t addr = 0;
        bool resolved = ResolveStereoModeAddress(&addr);

        if (enabling) {
            if (resolved && !g_haveOriginalValue.load(std::memory_order_relaxed)) {
                int32_t current = 0;
                if (SafeReadInt32(addr, &current)) {
                    g_originalStereoValue.store(current, std::memory_order_relaxed);
                    g_haveOriginalValue.store(true, std::memory_order_relaxed);
                }
            }
            modlog::Line(std::string("StereoMode override: ENABLED (forcing value=2)")
                + (resolved ? "" : " -- WARNING: pointer chain did not resolve, nothing will happen"));
        } else {
            if (resolved && g_haveOriginalValue.load(std::memory_order_relaxed)) {
                SafeWriteInt32(addr, g_originalStereoValue.load(std::memory_order_relaxed));
            }
            modlog::Line("StereoMode override: disabled (restored original value)");
        }
    }

    if (g_stereoOverrideEnabled.load(std::memory_order_relaxed)) {
        uintptr_t addr = 0;
        if (ResolveStereoModeAddress(&addr)) {
            SafeWriteInt32(addr, kStereoValue3DVision);
        }
    }
}

} // namespace vrmod
