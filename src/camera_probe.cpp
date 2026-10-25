#include "camera_probe.h"

#include <windows.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <iterator>
#include <cstdint>

#include "logger.h"

namespace vrmod {

namespace {

// Offsets from the SOTTR.exe module base, found via Cheat Engine's
// unknown-initial-value -> changed/unchanged -> increased/decreased float
// scan narrowing (25M -> 129 candidates), filtered down to the subset Cheat
// Engine resolved as static (module-relative) rather than heap addresses.
// Grouped by the stride pattern observed between addresses in each cluster.
struct Candidate {
    uintptr_t offset;
    const char* label;
};

// 11 addresses, each exactly 8 bytes apart -- a small fixed-size array or
// struct, the most promising singleton-style candidate (camera/player is
// usually one object, not an array).
constexpr Candidate kClusterA[] = {
    {0x204BE50, "A0"}, {0x204BE58, "A1"}, {0x204BE60, "A2"}, {0x204BE68, "A3"},
    {0x204BE70, "A4"}, {0x204BE78, "A5"}, {0x204BE80, "A6"}, {0x204BE88, "A7"},
    {0x204BE90, "A8"}, {0x204BE98, "A9"}, {0x204BEA0, "A10"},
};

// Standalone.
constexpr Candidate kSingle = {0x20BE2F8, "B0"};

// 4 addresses, each exactly 0x238 bytes apart -- looks like the same field
// across multiple identical struct instances (e.g. an entity array).
constexpr Candidate kClusterC[] = {
    {0x221057C, "C0"}, {0x22107B4, "C1"}, {0x22109EC, "C2"}, {0x2210C24, "C3"},
};

// 2 addresses, exactly 2x the cluster C stride apart -- likely the same
// struct array as C, just with gaps where the value didn't match our filter.
constexpr Candidate kClusterD[] = {
    {0x223C4C0, "D0"}, {0x223C930, "D1"},
};

// Reads a float from an absolute address, guarding against the address
// having become invalid (module layout differs from when Cheat Engine found
// it, or it's simply garbage) so a bad read can't crash the game.
bool SafeReadFloat(uintptr_t address, float* outValue) {
    __try {
        *outValue = *reinterpret_cast<float*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ProbeGroup(uintptr_t moduleBase, const Candidate* candidates, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uintptr_t addr = moduleBase + candidates[i].offset;
        float value = 0.0f;
        std::ostringstream oss;
        oss << "PROBE " << candidates[i].label
            << " off=0x" << std::hex << candidates[i].offset << std::dec << " ";
        if (SafeReadFloat(addr, &value)) {
            oss << std::fixed << std::setprecision(6) << value;
        } else {
            oss << "READ_FAILED";
        }
        modlog::Line(oss.str());
    }
}

} // namespace

void ProbeStaticCandidatesIfHotkeyDown() {
    if (!(GetAsyncKeyState(VK_F10) & 0x8000)) {
        return;
    }

    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!moduleBase) {
        return;
    }

    ProbeGroup(moduleBase, kClusterA, std::size(kClusterA));
    ProbeGroup(moduleBase, &kSingle, 1);
    ProbeGroup(moduleBase, kClusterC, std::size(kClusterC));
    ProbeGroup(moduleBase, kClusterD, std::size(kClusterD));
}

} // namespace vrmod
