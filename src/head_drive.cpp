#include "build_config.h"
#include "head_drive.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "aer.h"
#include "audio_cue.h"
#include "bone_scan.h"
#include "camera_interceptor.h"
#include "body_turn.h"
#include "face_track.h"
#include "logger.h"
#include "settings.h"

namespace vrmod {

// Workstream A, the real fix (docs/AIM.md, 2026-10-02): the head turns the game camera itself. The camera controllers
// keep their yaw/pitch in a small block (CAMSTATE + write test): ExplorationCameraController +0x12D4 (yaw, radians;
// +0x12D8 its twin) and +0x12EC (pitch, radians, negated; +0x12F0); AimCameraController the same layout at +0x904.
// Per controller, every game frame:
//   body += state - lastWritten     (what the game changed since our write: the stick, clamps, a re-sync)
//   state = body + head             (written to both twins; the next frame's camera includes the head)
// and the view is built from the body, not from the game camera: q_body = Rz(yaw_body - yaw_game) * q_game *
// Rx(pitch_body - pitch_game), with body = game - (state - body) of the dominant controller -- exact whatever the game
// clamps -- and AER then adds the full head pose as before. So the game's aim, rope auto-target, Lara's facing and
// world prompts follow the head, while the view never depends on the game's pitch limits or its one-frame lag.
// First person only (in third person the camera orbits Lara, so a head-turned camera would swing around her);
// elsewhere head aim (A3) stays in charge.
namespace {

struct Ctl {
    uintptr_t ptr = 0;
    uint32_t block = 0;      // offset of the yaw/pitch block
    bool active = false;     // written last frame
    float bodyY = 0, bodyP = 0;          // state units (pitch negated)
    float wroteY = 0, wroteP = 0;
    uint64_t lastFrame = 0;
};

Ctl g_ctl[6];
std::atomic<bool> g_on{false};
std::atomic<bool> g_active{false};
float g_hY = 0, g_hP = 0;     // head yaw/pitch applied with the last writes (radians)
std::atomic<uint64_t> g_frames{0}, g_writes{0}, g_resyncs{0};
std::atomic<int> g_maxDeltaCdeg{0};
// The game writes the camera twice per frame with identical values: the controllers are read and written at the
// first write; the second reuses the first's view correction.
uint64_t g_doneFrame = 0;
float g_offY = 0, g_offP = 0;
// Aim alignment (2026-10-09, user: before the bow is drawn the reticle only turns red / rope with the crosshair off the
// target; on charging the view jumps to where Lara aims, and from then on it's right). The game's targeting ray comes
// from stack 2's record (+0x930) whenever stack 2 has any weight (docs/AIM.md, rope arrows), but until the bow is drawn
// the view is ~98 % stack 1 -- and the aim controller's yaw/pitch are relative to its own reference, so body + head put
// it somewhere else. While the aim camera is in use but not dominant, its body is nudged by the difference between the
// view target written last frame and where its record actually points (the record shows last frame's write).
constexpr uintptr_t kAimRecord = 0x930, kRecordQuat = 0xA0;
float g_prevTargetY = 0, g_prevTargetP = 0;
bool g_havePrevTarget = false;
std::atomic<int> g_aimErrCdeg{0};          // largest aim-record error corrected (log)
// Rope-shot hold (2026-10-09): charging a rope arrow swaps the aim camera for a Simple camera head drive can't write. It
// points where Lara aimed -- the head turn included -- so letting go made the view add the head a second time (the
// crosshair jumped by the head angle). While that camera is in use the view keeps the head offset of the last driven
// frame taken off the game camera, and head aim (A3) takes the aim over with only the head's change since then.
bool g_ropeHold = false;
float g_ropeOffY = 0, g_ropeOffP = 0;     // the view correction frozen at the hold (radians)
std::atomic<bool> g_ropeHoldOn{false};
float g_ropeHead[4] = { 0, 0, 0, 1 };     // camera-local head turn at the hold (yaw about Z, then pitch about X)
std::mutex g_ropeMutex;
// The last driven frame (2026-10-09 run: the hold never engaged). The rope-shot flag comes from the Present thread a frame
// or two after the Simple camera takes over, and that first frame without a known controller released the drive (head
// part zeroed): the hold takes the values of the last driven frame if the rope camera shows up within 1 s of it.
ULONGLONG g_lastDriveMs = 0;
float g_lastHY = 0, g_lastHP = 0, g_lastOffY = 0, g_lastOffP = 0;
std::atomic<uint64_t> g_aimCorrections{0};

template <typename T>
bool Rd(uintptr_t a, T* out) {
    if (a < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const volatile T*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool Wr(uintptr_t a, T v) {
    if (a < 0x10000) return false;
    __try {
        *reinterpret_cast<volatile T*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t g_vtExplo = 0, g_vtAim = 0;

// RTTI class name check by vtable (cached once seen).
uint32_t BlockFor(uintptr_t ctrl) {
    uintptr_t vt = 0;
    if (!Rd(ctrl, &vt) || !vt) return 0;
    if (vt == g_vtExplo) return 0x12D4;
    if (vt == g_vtAim) return 0x904;
    uintptr_t col = 0;
    int32_t tdRva = 0;
    if (!Rd(vt - 8, &col) || !Rd(col + 0x0C, &tdRva)) return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char name[48] = {};
    for (int i = 0; i < 47; ++i) if (!Rd(base + static_cast<uint32_t>(tdRva) + 0x10 + i, &name[i]) || !name[i]) break;
    if (strcmp(name, ".?AVExplorationCameraController@@") == 0) { g_vtExplo = vt; return 0x12D4; }
    if (strcmp(name, ".?AVAimCameraController@@") == 0) { g_vtAim = vt; return 0x904; }
    return 0;
}

float Wrap(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

void QMul(const float a[4], const float b[4], float o[4]) {   // Hamilton, (x, y, z, w)
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// Puts every written controller back to its body (head part removed) and forgets them.
void Release(const char* why) {
    g_ropeHold = false;
    g_ropeHoldOn.store(false, std::memory_order_relaxed);
    bool any = false;
    for (Ctl& c : g_ctl) {
        if (c.active && c.ptr) {
            const uintptr_t b = c.ptr + c.block;
            float y = 0, p = 0;
            if (Rd(b, &y) && Rd(b + 0x18, &p)) {
                // Keep what the game did since our last write; take the head part off.
                const float ny = y - g_hY, np = p + g_hP;
                Wr(b, ny); Wr(b + 4, ny); Wr(b + 0x18, np); Wr(b + 0x1C, np);
            }
            any = true;
        }
        c = Ctl{};
    }
    g_hY = g_hP = 0;
    g_havePrevTarget = false;
    if (any && g_active.exchange(false)) modlog::Line(std::string("HEADDRIVE: released (") + why + ")");
    g_active.store(false);
}

// ---- Look limit (2026-10-10): the right stick could turn the first-person view 180 deg, looking back through Lara's own
// body. Her body's facing is her model root's (face draw World matrix, face_track.cpp); which local axis is her forward
// isn't known, so it is learned while she runs -- she faces where she runs: the axis (+X, -X, +Y, -Y) within 30 deg of
// her direction of travel in at least 80 % of 50 samples (one per 100 ms, moving over 1.5 m/s on the walking camera).
constexpr float kPi = 3.14159265f;
int g_lookAxis = -1;                 // learned forward axis: 0 +X, 1 -X, 2 +Y, 3 -Y; -1 not yet
int g_lookVotes[4] = {}, g_lookSamples = 0;
std::atomic<uint64_t> g_lookBlocked{0};
std::atomic<bool> g_lookingBack{false};
std::atomic<float> g_viewYawA{0.0f}, g_viewPitchA{0.0f};   // the view centre written last frame
std::atomic<uint64_t> g_turnCapped{0};
std::atomic<uint64_t> g_downCapped{0};   // frames whose stick look-down was stopped (log)   // frames whose stick turn was capped (log)
std::atomic<uint64_t> g_bodyHiddenFrames{0};
float g_lookWorst = 0;               // largest view-to-body angle seen (log), radians

bool LaraFacing(float* yaw) {
    float pos[3], ax[2];
    if (!FaceRootPose(pos, ax, 300.0)) return false;
    // Her model's -Y axis until learned otherwise: learned as -Y in every session so far (2026-10-10/11) and the
    // instance heading confirms it (body_turn.cpp); 2026-10-12 a session never ran enough to learn it and the body
    // never hid.
    const int axis = g_lookAxis >= 0 ? g_lookAxis : 3;
    *yaw = Wrap(ax[axis / 2] + ((axis & 1) ? kPi : 0.0f));
    return true;
}

void LookLimitSample(bool walking) {
    static float lastPos[3] = {};
    static ULONGLONG lastMs = 0;
    if (g_lookAxis >= 0 || (GetLookLimitDegSetting() <= 0.0f && GetHideBodyDegSetting() <= 0.0f)) return;
    float pos[3], ax[2];
    const ULONGLONG now = GetTickCount64();
    if (!walking || !FaceRootPose(pos, ax, 300.0)) { lastMs = 0; return; }
    if (lastMs == 0) { memcpy(lastPos, pos, sizeof pos); lastMs = now; return; }
    if (now - lastMs < 100) return;
    const float dx = pos[0] - lastPos[0], dy = pos[1] - lastPos[1];
    const float speed = std::sqrt(dx * dx + dy * dy) * 1000.0f / static_cast<float>(now - lastMs);   // cm/s
    memcpy(lastPos, pos, sizeof pos);
    lastMs = now;
    if (speed < 150.0f || speed > 1500.0f) return;   // walking / running, not standing or a teleport
    const float moveYaw = std::atan2(-dx, dy);
    for (int a = 0; a < 4; ++a) {
        const float cand = ax[a / 2] + ((a & 1) ? kPi : 0.0f);
        if (std::fabs(Wrap(cand - moveYaw)) < 0.5236f) ++g_lookVotes[a];
    }
    if (++g_lookSamples < 50) return;
    int best = 0;
    for (int a = 1; a < 4; ++a) if (g_lookVotes[a] > g_lookVotes[best]) best = a;
    static const char* kAxis[4] = { "+X", "-X", "+Y", "-Y" };
    char b[200];
    if (g_lookVotes[best] >= 40) {
        g_lookAxis = best;
        snprintf(b, sizeof(b), "LOOKLIMIT: Lara faces her model's %s axis (%d of %d running samples) -- stick limit %.0f deg, body hidden beyond %.0f deg (0 = off)",
                 kAxis[best], g_lookVotes[best], g_lookSamples, GetLookLimitDegSetting(), GetHideBodyDegSetting());
    } else {
        snprintf(b, sizeof(b), "LOOKLIMIT: no clear forward axis yet (+X %d, -X %d, +Y %d, -Y %d of %d) -- learning again",
                 g_lookVotes[0], g_lookVotes[1], g_lookVotes[2], g_lookVotes[3], g_lookSamples);
    }
    modlog::Line(b);
    for (int& v : g_lookVotes) v = 0;
    g_lookSamples = 0;
}

// The world-yaw correction for this frame: undoes the part of the stick's turn (stickDY) that took the view past the
// limit. Never moves the view otherwise -- she can turn away by herself (running towards the camera) without dragging it.
float LookLimitCorrection(float bodyYaw, float stickDY, bool walking) {
    const float limit = GetLookLimitDegSetting() * (kPi / 180.0f);
    float facing;
    if (limit <= 0.0f || !walking || !LaraFacing(&facing)) return 0.0f;
    const float e = Wrap(bodyYaw - facing);
    if (std::fabs(e) > g_lookWorst) g_lookWorst = std::fabs(e);
    if (std::fabs(e) <= limit || stickDY == 0.0f || (stickDY > 0.0f) != (e > 0.0f)) return 0.0f;
    const float undo = (std::min)(std::fabs(stickDY), std::fabs(e) - limit);
    g_lookBlocked.fetch_add(1, std::memory_order_relaxed);
    return e > 0.0f ? -undo : undo;
}

}  // namespace

bool HeadDriveActive() {
    return g_active.load(std::memory_order_relaxed);
}

bool HeadDriveViewAngles(float* yaw, float* pitch) {
    if (!g_active.load(std::memory_order_relaxed)) return false;
    *yaw = g_viewYawA.load(std::memory_order_relaxed);
    *pitch = g_viewPitchA.load(std::memory_order_relaxed);
    return true;
}

bool HeadDriveLookingBack() {
    return g_active.load(std::memory_order_relaxed) && g_lookingBack.load(std::memory_order_relaxed);
}

bool HeadDriveRopeHead(float q[4]) {
    if (!g_ropeHoldOn.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(g_ropeMutex);
    memcpy(q, g_ropeHead, sizeof g_ropeHead);
    return true;
}

bool HeadDriveGameThread(uintptr_t component, uint64_t gameFrame, float quat[4], bool eligible) {
    if (!g_on.load(std::memory_order_relaxed) || !eligible || !component) {
        if (g_active.load(std::memory_order_relaxed) || g_ctl[0].ptr || g_ropeHold) Release(!g_on.load() ? "off" : "not first person / suspended");
        return false;
    }
    if (gameFrame == g_doneFrame && g_active.load(std::memory_order_relaxed)) {
        const float zy[4] = { 0.0f, 0.0f, std::sin(-g_offY * 0.5f), std::cos(-g_offY * 0.5f) };
        const float xp[4] = { std::sin(-g_offP * 0.5f), 0.0f, 0.0f, std::cos(-g_offP * 0.5f) };
        float t[4], o[4];
        QMul(zy, quat, t);
        QMul(t, xp, o);
        memcpy(quat, o, sizeof o);
        return true;
    }
    float hd[4];
    if (!AerHeadAimDelta(hd)) {
        Release("no head views");
        return false;
    }
    // The controllers in use (entry +0x1C = used in this frame's blend): the base entry (+0xD20), stack 1 (lists
    // +0xF00 / +0xF18) and stack 2 (+0xF30 ... +0xF90). In ordinary play the active Exploration controller is usually
    // in stack 1 with the base entry unused (CAMMODE "base[Exploration] | stack1.1[Exploration*]", 2026-10-02: driving
    // the base entry only worked while aiming, when stack 2's Aim controller took over).
    uintptr_t used[6] = {};
    int usedStack[6] = {};   // 0 base, 1 stack 1, 2 stack 2
    float w2 = 0.0f;
    Rd(component + 0xC70, &w2);
    int n = 0;
    uintptr_t ctrl = 0;
    uint8_t isUsed = 0;
    if (Rd(component + 0xD20 + 0x20, &ctrl) && ctrl && Rd(component + 0xD20 + 0x1C, &isUsed) && isUsed) { usedStack[n] = 0; used[n++] = ctrl; }
    static const uint16_t kLists[7][2] = { { 0xF00, 0xF08 }, { 0xF18, 0xF20 }, { 0xF30, 0xF38 }, { 0xF48, 0xF50 },
                                           { 0xF60, 0xF68 }, { 0xF78, 0xF80 }, { 0xF90, 0xF98 } };
    for (int li = 0; li < 7; ++li) {
        uint32_t count = 0;
        uintptr_t arr = 0;
        if (!Rd(component + kLists[li][0], &count) || !Rd(component + kLists[li][1], &arr) || !arr) continue;
        for (uint32_t i = 0; i < count && i < 4 && n < 6; ++i) {
            isUsed = 0;
            if (Rd(arr + i * 0x1E0 + 0x20, &ctrl) && ctrl && Rd(arr + i * 0x1E0 + 0x1C, &isUsed) && isUsed) {
                bool dup = false;
                for (int k = 0; k < n; ++k) dup = dup || used[k] == ctrl;
                if (!dup) { usedStack[n] = li < 2 ? 1 : 2; used[n++] = ctrl; }
            }
        }
    }

    // Drop controllers no longer in use (their state is theirs again; a re-entry starts from what they hold).
    for (Ctl& c : g_ctl) {
        if (!c.ptr) continue;
        bool still = false;
        for (int k = 0; k < n; ++k) still = still || used[k] == c.ptr;
        if (!still) c = Ctl{};
    }

    // Pass 1, per controller: body += state - written (what the game changed since our write: stick, clamps, re-sync).
    Ctl* list[6] = {};
    int m = 0;
    Ctl* dominant = nullptr;
    int domRank = 9;
    float domStateY = 0, domStateP = 0, domDY = 0, domDP = 0;
    for (int k = 0; k < n; ++k) {
        const uint32_t block = BlockFor(used[k]);
        if (!block) continue;
        Ctl* c = nullptr;
        for (Ctl& s2 : g_ctl) if (s2.ptr == used[k]) c = &s2;
        const uintptr_t b = used[k] + block;
        float y = 0, p = 0;
        if (!Rd(b, &y) || !Rd(b + 0x18, &p)) continue;
        float dYk = 0, dPk = 0;   // the game's own turn this frame (stick, clamps) -- the look limits
        if (!c) {
            for (Ctl& s2 : g_ctl) if (!s2.ptr) { c = &s2; break; }
            if (!c) continue;
            c->ptr = used[k];
            c->block = block;
            // A controller joining (the aim camera at aim start) starts from the camera's last output, which already
            // carries the head applied last frame: its body is the state without that.
            c->bodyY = y - g_hY;
            c->bodyP = p + g_hP;
            c->wroteY = y;
            c->wroteP = p;
            g_resyncs.fetch_add(1, std::memory_order_relaxed);
        } else {
            const float dY = Wrap(y - c->wroteY), dP = p - c->wroteP;
            dYk = dY;
            dPk = dP;
            c->bodyY += dY;
            c->bodyP += dP;
            const int cd = static_cast<int>(std::fabs(dY) * 5729.578f);
            int prev = g_maxDeltaCdeg.load(std::memory_order_relaxed);
            while (cd > prev && !g_maxDeltaCdeg.compare_exchange_weak(prev, cd, std::memory_order_relaxed)) {}
        }
        list[m++] = c;
        // The view's body comes from the controller that dominates this frame's blend: stack 2 while its weight is
        // over 0.5 (the aim camera with the bow drawn), otherwise stack 1 / the base entry.
        const int rank = (w2 > 0.5f) ? (usedStack[k] == 2 ? 0 : 1) : (usedStack[k] == 2 ? 1 : 0);
        if (!dominant || rank < domRank) {
            dominant = c;
            domRank = rank;
            domStateY = y;
            domStateP = p;
            domDY = dYk;
            domDP = dPk;
        }
    }
    if (!dominant) {
        if (CameraRopeShot() && (g_ropeHold || (g_lastDriveMs && GetTickCount64() - g_lastDriveMs < 1000))) {
            if (!g_ropeHold) {
                Release("rope-shot camera");   // nothing to put back (the controllers were dropped); clears state
                g_ropeHold = true;
                g_hY = g_lastHY;   // kept: the dropped controllers' states still carry it (a re-join takes it off)
                g_hP = g_lastHP;
                g_ropeOffY = g_lastOffY;
                g_ropeOffP = g_lastOffP;
                {
                    std::lock_guard<std::mutex> lock(g_ropeMutex);
                    const float zy[4] = { 0.0f, 0.0f, std::sin(g_hY * 0.5f), std::cos(g_hY * 0.5f) };
                    const float xp[4] = { std::sin(g_hP * 0.5f), 0.0f, 0.0f, std::cos(g_hP * 0.5f) };
                    QMul(zy, xp, g_ropeHead);
                }
                g_ropeHoldOn.store(true, std::memory_order_relaxed);
                g_active.store(false);   // head aim's injection (relative, see HeadDriveRopeHead) takes the aim over
                for (Ctl& c : g_ctl) c = Ctl{};   // their states keep the head part; a re-join takes it off (g_hY kept)
                char b[160];
                snprintf(b, sizeof(b), "HEADDRIVE: rope-shot camera -- holding the head offset of the charge (yaw %.1f, pitch %.1f deg)", g_hY * 57.29578f, g_hP * 57.29578f);
                modlog::Line(b);
            }
            const float zy[4] = { 0.0f, 0.0f, std::sin(-g_ropeOffY * 0.5f), std::cos(-g_ropeOffY * 0.5f) };
            const float xp[4] = { std::sin(-g_ropeOffP * 0.5f), 0.0f, 0.0f, std::cos(-g_ropeOffP * 0.5f) };
            float t[4], o[4];
            QMul(zy, quat, t);
            QMul(t, xp, o);
            memcpy(quat, o, sizeof o);
            return true;
        }
        Release("no known camera controller in use");
        return false;
    }
    if (g_ropeHold) {
        g_ropeHold = false;
        g_ropeHoldOn.store(false, std::memory_order_relaxed);
        modlog::Line("HEADDRIVE: rope-shot camera over -- driving again");
    }

    // The body (world yaw / pitch): the game camera with the dominant controller's head part (state - body, as the
    // game used it this frame) taken off -- exact whatever the game clamped.
    const float qx = quat[0], qy = quat[1], qz = quat[2], qw = quat[3];
    const float gfx = 2.0f * (qx * qy - qw * qz), gfy = 1.0f - 2.0f * (qx * qx + qz * qz), gfz = 2.0f * (qy * qz + qw * qx);
    const float gameYaw = std::atan2(-gfx, gfy);
    const float gamePitch = std::asin(gfz < -1.0f ? -1.0f : (gfz > 1.0f ? 1.0f : gfz));
    float offY = Wrap(domStateY - dominant->bodyY);
    float offP = -(domStateP - dominant->bodyP);
    float bodyYaw = gameYaw - offY;
    float bodyPitch = gamePitch - offP;
    {
        // Look limit: the walking camera (not the aim camera, which turns her body with it) can't be turned further
        // from where her body faces than [firstperson] look_limit_deg -- only the stick's push past the limit is undone.
        const bool walking = dominant->block == 0x12D4 && w2 <= 0.5f;
        // Stick turn rate (stick_turn_max_deg_s): the part of this frame's stick turn beyond the cap is undone, on every
        // driven camera (walking and aim). Full-rate turns (~170 deg/s) blurred the image.
        {
            static LARGE_INTEGER lastQ{};
            LARGE_INTEGER q, f;
            QueryPerformanceCounter(&q);
            QueryPerformanceFrequency(&f);
            float dt = lastQ.QuadPart ? static_cast<float>(q.QuadPart - lastQ.QuadPart) / static_cast<float>(f.QuadPart) : 0.0f;
            lastQ = q;
            dt = dt < 1.0f / 144.0f ? 1.0f / 144.0f : (dt > 1.0f / 30.0f ? 1.0f / 30.0f : dt);
            const float cap = GetStickTurnMaxDegSetting() * (kPi / 180.0f) * dt;
            if (cap > 0.0f && std::fabs(domDY) > cap) {
                const float c = std::copysign(cap, domDY) - domDY;
                for (int k = 0; k < m; ++k) list[k]->bodyY += c;
                domDY += c;
                offY = Wrap(domStateY - dominant->bodyY);
                bodyYaw = gameYaw - offY;
                g_turnCapped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Looking down (look_down_max_deg, 2026-10-12): the stick can't take the view -- stick and real head together --
        // further below horizontal than the limit; looking down into her body showed its inside. Only the stick's
        // downward push past it is undone; the real head is never pushed back (the view follows a head that looks
        // further down). World pitch: up positive; the state's pitch is negated, so the stick's world change is -dP.
        {
            const float lim = -GetLookDownMaxDegSetting() * (kPi / 180.0f);
            const float combined = bodyPitch + g_hP;   // the head's pitch from the last frame
            const float stickWorld = -domDP;
            if (walking && lim < 0.0f && combined < lim && stickWorld < 0.0f) {
                const float raise = (std::min)(-stickWorld, lim - combined);
                for (int k = 0; k < m; ++k) list[k]->bodyP -= raise;
                offP = -(domStateP - dominant->bodyP);
                bodyPitch = gamePitch - offP;
                g_downCapped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        LookLimitSample(walking);
        const float corr = LookLimitCorrection(bodyYaw, domDY, walking);
        if (corr != 0.0f) {
            for (int k = 0; k < m; ++k) list[k]->bodyY += corr;
            offY = Wrap(domStateY - dominant->bodyY);
            bodyYaw = gameYaw - offY;
        }
        // Body turn (body_turn.cpp): standing still, her body follows the view the stick turned.
        BodyTurnGameThread(bodyYaw, walking);
    }

    // Where the view centre will point: body * head (the head's yaw/pitch d, as AER composes it), as world yaw / pitch;
    // the game camera is turned by the difference from the body.
    const float by[4] = { 0.0f, 0.0f, std::sin(bodyYaw * 0.5f), std::cos(bodyYaw * 0.5f) };
    const float bp[4] = { std::sin(bodyPitch * 0.5f), 0.0f, 0.0f, std::cos(bodyPitch * 0.5f) };
    float qb[4], qv[4];
    QMul(by, bp, qb);
    QMul(qb, hd, qv);
    const float vfx = 2.0f * (qv[0] * qv[1] - qv[3] * qv[2]), vfy = 1.0f - 2.0f * (qv[0] * qv[0] + qv[2] * qv[2]),
                vfz = 2.0f * (qv[1] * qv[2] + qv[3] * qv[0]);
    const float hY = Wrap(std::atan2(-vfx, vfy) - bodyYaw);
    const float hP = std::asin(vfz < -1.0f ? -1.0f : (vfz > 1.0f ? 1.0f : vfz)) - bodyPitch;

    // Aim alignment: where stack 2's record points now (last frame's write) vs the view target written then.
    float aimErrY = 0, aimErrP = 0;
    bool aimCorrect = false;
    if (g_havePrevTarget && w2 > 0.0f && w2 <= 0.5f) {
        float rq[4];
        if (Rd(component + kAimRecord + kRecordQuat, &rq[0]) && Rd(component + kAimRecord + kRecordQuat + 4, &rq[1])
            && Rd(component + kAimRecord + kRecordQuat + 8, &rq[2]) && Rd(component + kAimRecord + kRecordQuat + 12, &rq[3])) {
            const float nq = rq[0] * rq[0] + rq[1] * rq[1] + rq[2] * rq[2] + rq[3] * rq[3];
            if (nq > 0.9f && nq < 1.1f) {
                const float rfx = 2.0f * (rq[0] * rq[1] - rq[3] * rq[2]), rfy = 1.0f - 2.0f * (rq[0] * rq[0] + rq[2] * rq[2]),
                            rfz = 2.0f * (rq[1] * rq[2] + rq[3] * rq[0]);
                const float rY = std::atan2(-rfx, rfy), rP = std::asin(rfz < -1.0f ? -1.0f : (rfz > 1.0f ? 1.0f : rfz));
                aimErrY = Wrap(g_prevTargetY - rY);
                aimErrP = g_prevTargetP - rP;
                aimCorrect = std::fabs(aimErrY) < 0.6f && std::fabs(aimErrP) < 0.6f;   // ~35 deg: beyond that, not this
            }
        }
    }

    // Pass 2: write body + head into every controller (both twins; pitch negated).
    for (int k = 0; k < m; ++k) {
        Ctl* c = list[k];
        if (aimCorrect && c->block == 0x904) {
            // Half the error per frame: the stick's own turn shows up in the record a frame later, so a full step overshoots.
            c->bodyY += 0.5f * aimErrY;
            c->bodyP -= 0.5f * aimErrP;   // state pitch is negated
            const int cd = static_cast<int>((std::max)(std::fabs(aimErrY), std::fabs(aimErrP)) * 5729.578f);
            int prev = g_aimErrCdeg.load(std::memory_order_relaxed);
            while (cd > prev && !g_aimErrCdeg.compare_exchange_weak(prev, cd, std::memory_order_relaxed)) {}
            g_aimCorrections.fetch_add(1, std::memory_order_relaxed);
        }
        const uintptr_t b = c->ptr + c->block;
        const float ny = c->bodyY + hY, np = c->bodyP - hP;
        Wr(b, ny); Wr(b + 4, ny); Wr(b + 0x18, np); Wr(b + 0x1C, np);
        c->wroteY = ny;
        c->wroteP = np;
        c->active = true;
        c->lastFrame = gameFrame;
        g_writes.fetch_add(1, std::memory_order_relaxed);
    }
    g_hY = hY;
    g_hP = hP;
    {
        // Looking back (hide_body_deg): the view's world yaw -- body and real head -- against where her body faces.
        const float hideAt = GetHideBodyDegSetting() * (kPi / 180.0f);
        float facing;
        const float downAt = -GetHideBodyDownDegSetting() * (kPi / 180.0f);
        bool back = false;
        // Looking steeply down too (hide_body_down_deg): the camera sits in her head, so the view went down through her
        // neck and chest from inside (2026-10-12). View pitch = body + real head, up positive.
        if (downAt < 0.0f && dominant->block == 0x12D4 && w2 <= 0.5f)
            back = bodyPitch + hP < (g_lookingBack.load(std::memory_order_relaxed) ? downAt + 0.1745f : downAt);
        if (!back && hideAt > 0.0f && dominant->block == 0x12D4 && w2 <= 0.5f && LaraFacing(&facing)) {
            const float a = std::fabs(Wrap(bodyYaw + hY - facing));
            if (a > g_lookWorst) g_lookWorst = a;
            back = a > (g_lookingBack.load(std::memory_order_relaxed) ? hideAt - 0.1745f : hideAt);
        }
        g_lookingBack.store(back, std::memory_order_relaxed);
        if (back) g_bodyHiddenFrames.fetch_add(1, std::memory_order_relaxed);
    }
    g_prevTargetY = Wrap(bodyYaw + hY);   // the view centre written this frame (world yaw / pitch)
    g_viewYawA.store(g_prevTargetY, std::memory_order_relaxed);
    g_viewPitchA.store(bodyPitch + hP, std::memory_order_relaxed);
    g_prevTargetP = bodyPitch + hP;
    g_havePrevTarget = true;
    if (!g_active.exchange(true)) modlog::Line("HEADDRIVE: the head now turns the game camera (first person).");
    g_frames.fetch_add(1, std::memory_order_relaxed);

    // View base: the body rotation (yaw about world Z, pitch about the camera's own X); AER adds the full head pose.
    g_offY = offY;
    g_offP = offP;
    g_doneFrame = gameFrame;
    g_lastDriveMs = GetTickCount64();
    g_lastHY = hY;
    g_lastHP = hP;
    g_lastOffY = offY;
    g_lastOffP = offP;
    const float zy[4] = { 0.0f, 0.0f, std::sin(-offY * 0.5f), std::cos(-offY * 0.5f) };
    const float xp[4] = { std::sin(-offP * 0.5f), 0.0f, 0.0f, std::cos(-offP * 0.5f) };
    float t[4], o[4];
    QMul(zy, quat, t);
    QMul(t, xp, o);
    memcpy(quat, o, sizeof o);
    return true;
}

void HeadDriveOnPresent() {
    static bool loaded = false;
    if (!loaded) { loaded = true; g_on.store(GetHeadDriveSetting()); }
    static bool eWas = false;
    const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool e = (VRMOD_DEV_KEY('E') & 0x8000) != 0;
    if (e && !eWas && ca) HeadDriveToggle("Ctrl+Alt+E");
    eWas = e;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 30000) {
        last = now;
        const uint64_t f = g_frames.exchange(0), w = g_writes.exchange(0), r = g_resyncs.exchange(0);
        if (g_on.load() || f) {
            char b[320];
            snprintf(b, sizeof(b), "HEADDRIVE (30 s, %s): %llu frames driven, %llu controller writes, %llu controllers joined, largest per-frame stick yaw %.2f deg; head now yaw %.1f pitch %.1f deg; aim camera aligned to the view %llu times (largest error %.2f deg)",
                     g_on.load() ? (g_active.load() ? "on, active" : "on, idle") : "off", static_cast<unsigned long long>(f),
                     static_cast<unsigned long long>(w), static_cast<unsigned long long>(r), g_maxDeltaCdeg.exchange(0) / 100.0f,
                     g_hY * 57.29578f, g_hP * 57.29578f, static_cast<unsigned long long>(g_aimCorrections.exchange(0)), g_aimErrCdeg.exchange(0) / 100.0f);
            modlog::Line(b);
            if (GetLookLimitDegSetting() > 0.0f || GetHideBodyDegSetting() > 0.0f) {
                static const char* kAxis[4] = { "+X", "-X", "+Y", "-Y" };
                snprintf(b, sizeof(b), "LOOKLIMIT (30 s): stick limit %.0f deg, body hidden beyond %.0f deg; forward axis %s; stick held at the limit in %llu frames, body hidden in %llu frames, largest view-to-body angle %.0f deg",
                         GetLookLimitDegSetting(), GetHideBodyDegSetting(), g_lookAxis >= 0 ? kAxis[g_lookAxis] : "not learned yet",
                         static_cast<unsigned long long>(g_lookBlocked.exchange(0)), static_cast<unsigned long long>(g_bodyHiddenFrames.exchange(0)),
                         g_lookWorst * 57.29578f);
                g_lookWorst = 0;
                modlog::Line(b);
            }
            if (GetBodyTurnDegSetting() > 0.0f) modlog::Line(BodyTurnStats());
            if (GetStickTurnMaxDegSetting() > 0.0f) {
                snprintf(b, sizeof(b), "STICKTURN (30 s): capped at %.0f deg/s in %llu frames; looking down stopped at %.0f deg in %llu frames",
                         GetStickTurnMaxDegSetting(), static_cast<unsigned long long>(g_turnCapped.exchange(0)), GetLookDownMaxDegSetting(),
                         static_cast<unsigned long long>(g_downCapped.exchange(0)));
                modlog::Line(b);
            }
        }
    }
}

bool HeadDriveEnabled() {
    return g_on.load(std::memory_order_relaxed);
}

void HeadDriveToggle(const char* how) {
    const bool on = !g_on.load();
    g_on.store(on);
    modlog::Line(std::string("HEADDRIVE: head drives the game camera ") + (on ? "ON" : "off") + " (" + how + "; first person; head aim covers the rest)");
    PlayCue(on ? Cue::On : Cue::Off);
}

}  // namespace vrmod
