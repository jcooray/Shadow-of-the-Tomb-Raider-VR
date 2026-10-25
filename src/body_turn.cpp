#include "game_addresses.h"
#include "body_turn.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "build_config.h"
#include "face_track.h"
#include "logger.h"
#include "settings.h"

namespace vrmod {

namespace {

constexpr float kPi = 3.14159265f;
// The world-object pool (DAT_14301d0c0 in the Steam build: 20000 entries of 0x120 bytes; position at +0x10, angles
// about X/Y/Z at +0x30, radians) and the component index the Player is found with (DAT_1416b99c8): player =
// *(*(*(inst + 0xD8) + index * 8) + 0x2A8), and *(player + 0x33E8) points back at the instance (docs/FIRSTPERSON.md,
// "Body turn"). Both found by pattern (game_addresses.cpp, 2026-10-18); the struct offsets are build-dependent, but
// IsLara's back-pointer check means a different layout finds no Lara instead of writing to the wrong object.
constexpr uint32_t kPoolCount = 20000, kStride = 0x120;

uint64_t g_pool = 0, g_indexAddr = 0;
bool g_inited = false, g_supported = false;
uint64_t g_inst = 0;
int g_instIndex = -1;
std::atomic<bool> g_turning{false};
float g_lastPos[3] = {};
LARGE_INTEGER g_lastQpc{};
std::atomic<uint32_t> g_turns{0};
std::atomic<int> g_lastTurnCdeg{0};
std::atomic<uint64_t> g_turnFrames{0};
std::atomic<uint32_t> g_finds{0};

float Wrap(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

bool SafeCopy(void* dst, uint64_t src, size_t n) {
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SafeWrite(uint64_t dst, const void* src, size_t n) {
    __try {
        memcpy(reinterpret_cast<void*>(dst), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Init() {
    g_inited = true;
    g_pool = GameAddress(GameAddr::InstancePool);
    g_indexAddr = GameAddress(GameAddr::PlayerIndexGlobal);
    if (!g_pool || !g_indexAddr) {
        modlog::Line("BODYTURN: the world-object pool or the player index wasn't found by pattern -- body turning off");
        return;
    }
    g_supported = true;
}

// The instance is Lara's: the Player component hangs off it and points back at it.
bool IsLara(uint64_t inst) {
    uint32_t index = 0;
    uint64_t table = 0, comp = 0, player = 0, back = 0;
    return SafeCopy(&index, g_indexAddr, 4) && index < 4096 && SafeCopy(&table, inst + 0xD8, 8) && table
           && SafeCopy(&comp, table + static_cast<uint64_t>(index) * 8, 8) && comp && SafeCopy(&player, comp + 0x2A8, 8) && player
           && SafeCopy(&back, player + 0x33E8, 8) && back == inst;
}

// The pool entry near her drawn model that is Lara (her instance stands at her feet, ~90 cm under the model root).
bool Find(const float root[3]) {
    for (uint32_t i = 0; i < kPoolCount; ++i) {
        const uint64_t inst = g_pool + static_cast<uint64_t>(i) * kStride;
        float p[3];
        if (!SafeCopy(p, inst + 0x10, 12)) continue;
        if (std::fabs(p[0] - root[0]) > 100.0f || std::fabs(p[1] - root[1]) > 100.0f || std::fabs(p[2] - root[2]) > 250.0f) continue;
        if (!IsLara(inst)) continue;
        g_inst = inst;
        g_instIndex = static_cast<int>(i);
        g_finds.fetch_add(1, std::memory_order_relaxed);
        char b[120];
        snprintf(b, sizeof(b), "BODYTURN: Lara is world object %u -- her body turns with the view (beyond %.0f deg, %.0f deg/s)", i,
                 GetBodyTurnDegSetting(), GetBodyTurnSpeedSetting());
        modlog::Line(b);
        return true;
    }
    return false;
}

#if VRMOD_DIAG
// Gravity measurement (development build, 2026-10-27): her position (the world object at her feet, game units) at every
// camera update to vrmod_fall.csv, for 15 minutes from her object being found. Her falls fitted offline give the game's
// gravity in units/s^2; against 9.81 m/s^2 that is its units per metre, independent of her height (the true scale).
void FallLog(const LARGE_INTEGER& now, const LARGE_INTEGER& freq) {
    static FILE* f = nullptr;
    static bool done = false;
    static LONGLONG start = 0;
    static float last[3] = {};
    static uint32_t rows = 0;
    if (done || !g_inst || !IsLara(g_inst)) return;
    float p[3];
    if (!SafeCopy(p, g_inst + 0x10, 12)) return;
    if (!f) {
        if (fopen_s(&f, "vrmod_fall.csv", "w") != 0 || !f) { done = true; return; }
        fprintf(f, "t_s,x,y,z\n");
        start = now.QuadPart;
        modlog::Line("FALLLOG: writing Lara's position at every camera update to vrmod_fall.csv (15 minutes) for the gravity measurement");
    }
    const double t = static_cast<double>(now.QuadPart - start) / static_cast<double>(freq.QuadPart);
    if (t > 900.0) {
        fclose(f);
        f = nullptr;
        done = true;
        modlog::Line("FALLLOG: done (" + std::to_string(rows) + " rows)");
        return;
    }
    if (memcmp(p, last, sizeof p) == 0) return;   // the same game frame again
    memcpy(last, p, sizeof p);
    fprintf(f, "%.5f,%.2f,%.2f,%.2f\n", t, p[0], p[1], p[2]);
    if (++rows % 72 == 0) fflush(f);
}
#endif

}  // namespace

void BodyTurnGameThread(float viewYaw, bool walkingCamera) {
    if (!g_inited) Init();
    const float startAt = GetBodyTurnDegSetting() * (kPi / 180.0f);
    if (!g_supported || startAt <= 0.0f) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_lastQpc.QuadPart ? static_cast<float>(now.QuadPart - g_lastQpc.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    g_lastQpc = now;
    if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / 72.0f;
#if VRMOD_DIAG
    FallLog(now, freq);
#endif
    if (!walkingCamera) { g_turning = false; return; }
    // Her instance: kept while it is still hers; looked up again (at most every 2 s) otherwise.
    if (g_inst && !IsLara(g_inst)) { g_inst = 0; g_turning = false; }
    if (!g_inst) {
        static ULONGLONG lastTry = 0;
        const ULONGLONG t = GetTickCount64();
        float root[3], ax[2];
        if (t - lastTry < 2000 || !FaceRootPose(root, ax, 250.0)) return;
        lastTry = t;
        if (!Find(root)) return;
    }
    float pos[3], ang[4];
    if (!SafeCopy(pos, g_inst + 0x10, 12) || !SafeCopy(ang, g_inst + 0x30, 16)) { g_inst = 0; return; }
    // Moving (the game turns her towards where she goes) or not upright: leave her be.
    const float dx = pos[0] - g_lastPos[0], dy = pos[1] - g_lastPos[1];
    memcpy(g_lastPos, pos, sizeof pos);
    const bool moving = std::sqrt(dx * dx + dy * dy) / dt > 30.0f;   // cm/s
    if (moving || std::fabs(ang[0]) > 0.2f || std::fabs(ang[1]) > 0.2f) { g_turning = false; return; }
    // Her facing: her model's -Y axis, whose yaw is the instance's Z angle + 180 deg.
    const float facing = Wrap(ang[2] + kPi);
    const float e = Wrap(viewYaw - facing);
    if (!g_turning && std::fabs(e) > startAt) {
        g_turning = true;
        g_turns.fetch_add(1, std::memory_order_relaxed);
        g_lastTurnCdeg.store(static_cast<int>(e * 5729.578f), std::memory_order_relaxed);
    }
    if (!g_turning) return;
    // Eased (~0.12 s time constant), at most body_turn_speed deg/s, at least 40 deg/s so it finishes.
    const float maxStep = GetBodyTurnSpeedSetting() * (kPi / 180.0f) * dt;
    const float minStep = 40.0f * (kPi / 180.0f) * dt;
    float step = e * (std::min)(1.0f, 8.0f * dt);
    if (std::fabs(step) > maxStep) step = std::copysign(maxStep, e);
    if (std::fabs(step) < minStep) step = std::copysign((std::min)(minStep, std::fabs(e)), e);
    const float z = Wrap(ang[2] + step);
    SafeWrite(g_inst + 0x38, &z, 4);
    g_turnFrames.fetch_add(1, std::memory_order_relaxed);
    if (std::fabs(e - step) < 3.0f * (kPi / 180.0f)) g_turning = false;
}

bool BodyTurnTurning() {
    return g_turning.load(std::memory_order_relaxed);
}

std::string BodyTurnStats() {
    char b[200];
    if (!g_inited) return "BODYTURN (30 s): waiting for first-person gameplay";
    if (!g_supported) return "BODYTURN: off (unknown SOTTR.exe build)";
    snprintf(b, sizeof(b), "BODYTURN (30 s): start beyond %.0f deg, %.0f deg/s; Lara %s; %u turns (last %+.0f deg), %llu frames turning",
             GetBodyTurnDegSetting(), GetBodyTurnSpeedSetting(), g_inst ? "found" : "not found yet", g_turns.exchange(0),
             g_lastTurnCdeg.load() / 100.0f, static_cast<unsigned long long>(g_turnFrames.exchange(0)));
    return b;
}

}  // namespace vrmod
