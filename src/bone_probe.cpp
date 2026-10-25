#include "bone_probe.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "logger.h"
#include "present_hook.h"

namespace vrmod {

namespace {

// cdc::BaseInstanceBoneAnchor's position-resolving vmethod (vtable slot 3, found by dumping the class's vtable).
// It reads the same global per-joint table our CameraBoneController probe reads (see cambone_probe.cpp): a handle
// at thisPtr+8 is looked up in a hash table to get a skeleton-instance index, validated, then combined with the
// bone index at thisPtr+0x10 to fetch a 0x40-byte-stride bone record from that skeleton's transform array.
constexpr uintptr_t kAccessorRva = 0x435BF0;
constexpr unsigned char kAccessorPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0xDA,
    0x4C, 0x3B, 0x05, 0x2C, 0x30, 0xBD, 0x02, 0x4C, 0x8B, 0xD1, 0x74
};

using Accessor_t = uint64_t*(*)(void* thisPtr, uint64_t* outPos);
Accessor_t g_origAccessor = nullptr;

double NowMs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

struct PairState { double lastLogMs = 0.0; uint64_t calls = 0; };
struct HandleState { uint64_t calls = 0; float maxZ = -1e9f; int32_t maxZBone = -1; double lastSummaryMs = 0.0; };

std::mutex g_mutex;
std::unordered_map<uint64_t, PairState> g_pairs;      // key = handle ^ (boneIndex << 40)
std::unordered_map<uint64_t, HandleState> g_handles;
uint64_t g_totalCalls = 0;

uint64_t PairKey(uint64_t handle, int32_t bone) {
    return handle ^ (static_cast<uint64_t>(static_cast<uint32_t>(bone)) << 40);
}

// Isolated to a plain function with no C++ objects in scope, so it's safe to wrap in SEH (__try can't share a
// function with objects that need unwinding, e.g. std::lock_guard/std::ostringstream -- those live in the caller).
bool ExtractSafely(void* thisPtr, const uint64_t* outPos, uint64_t* outHandle, int32_t* outBone, float pos[4]) {
    __try {
        *outHandle = *reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(thisPtr) + 8);
        *outBone = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(thisPtr) + 0x10);
        const float* f = reinterpret_cast<const float*>(outPos);
        pos[0] = f[0]; pos[1] = f[1]; pos[2] = f[2]; pos[3] = f[3];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;   // never take the game down over a probe
    }
}

std::atomic<uint64_t> g_callsZero{0}, g_callsNonzero{0};
double g_lastHeartbeatMs = 0.0;
std::mutex g_heartbeatMutex;

uint64_t* hkAccessor(void* thisPtr, uint64_t* outPos) {
    uint64_t* ret = g_origAccessor(thisPtr, outPos);

    // Unconditional heartbeat, independent of everything below: settles whether this hook is on the right
    // function and reached at all, versus reached but silently discarded by the handle==0 filter (or a wrong
    // field-offset assumption). Logged even if the rest of this function is somehow wrong.
    {
        const uint64_t handlePeek = *reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(thisPtr) + 8);
        (handlePeek == 0 ? g_callsZero : g_callsNonzero).fetch_add(1, std::memory_order_relaxed);
        const double nowHb = NowMs();
        if (nowHb - g_lastHeartbeatMs >= 2000.0) {
            std::lock_guard<std::mutex> hbLock(g_heartbeatMutex);
            if (nowHb - g_lastHeartbeatMs >= 2000.0) {
                g_lastHeartbeatMs = nowHb;
                std::ostringstream hb;
                hb << "BoneAccessorProbe heartbeat: calls with handle==0: " << g_callsZero.load()
                   << ", handle!=0: " << g_callsNonzero.load();
                modlog::Line(hb.str());
            }
        }
    }

    if (g_totalCalls++ > 20000000ull) {
        return ret;   // long-running safety valve: stop the bookkeeping, keep passing calls through
    }
    uint64_t handle = 0;
    int32_t bone = 0;
    float pos[4] = {};
    if (!ExtractSafely(thisPtr, outPos, &handle, &bone, pos) || handle == 0) {
        return ret;
    }
    const float x = pos[0], y = pos[1], z = pos[2], w = pos[3];

    const double now = NowMs();
    std::lock_guard<std::mutex> lock(g_mutex);

    HandleState& hs = g_handles[handle];
    ++hs.calls;
    if (z > hs.maxZ) { hs.maxZ = z; hs.maxZBone = bone; }

    PairState& ps = g_pairs[PairKey(handle, bone)];
    const bool firstSeen = ps.calls == 0;
    ++ps.calls;
    if (firstSeen || now - ps.lastLogMs >= 2000.0) {
        ps.lastLogMs = now;
        std::ostringstream o;
        o << "BONEACC handle=0x" << std::hex << handle << std::dec << " bone=" << bone
          << " pos=(" << x << "," << y << "," << z << "," << w << ") calls=" << hs.calls;
        modlog::Line(o.str());
    }

    if (g_pairs.size() < 4000 && now - hs.lastSummaryMs >= 3000.0 && hs.calls > 50) {
        hs.lastSummaryMs = now;
        std::ostringstream o;
        o << "BONESUMMARY handle=0x" << std::hex << handle << std::dec << " calls=" << hs.calls
          << " highestBone=" << hs.maxZBone << " atZ=" << hs.maxZ;
        modlog::Line(o.str());
    }
    return ret;
}

} // namespace

bool InstallBoneAccessorProbe() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        return false;
    }
    void* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(exe) + kAccessorRva);
    if (memcmp(target, kAccessorPrologue, sizeof(kAccessorPrologue)) != 0) {
        modlog::Line("BoneAccessorProbe: prologue mismatch at the expected RVA (different build?); probe off.");
        return false;
    }
    if (!EnsureMinHookInitialized()) {
        return false;
    }
    if (MH_CreateHook(target, &hkAccessor, reinterpret_cast<void**>(&g_origAccessor)) != MH_OK
        || MH_EnableHook(target) != MH_OK) {
        modlog::Line("BoneAccessorProbe: hook install failed.");
        return false;
    }
    modlog::Line("BoneAccessorProbe: installed. Logs BONEACC (per handle+bone, throttled) and periodic "
        "BONESUMMARY lines (each handle's highest-Z bone seen) -- just play normally, no cutscene needed.");
    return true;
}

}
