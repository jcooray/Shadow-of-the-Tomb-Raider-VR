#include "build_config.h"
#include "camera_interceptor.h"
#include "bone_scan.h"
#include "gear_hide.h"
#include "near_probe.h"

#include <windows.h>
#include <MinHook.h>
#include <Xinput.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "game_addresses.h"
#include "logger.h"
#include "head_tracking.h"
#include "settings.h"
#include "audio_cue.h"
#include "stereo_scale.h"
#include "frame_log.h"
#include "aer.h"
#include "cinematic_watch.h"
#include "perf_stats.h"
#include "openxr_integration.h"
#include "ui_probe.h"
#include "cam_state_probe.h"
#include "head_drive.h"
#include "face_track.h"
#include "facing_probe.h"
#include "body_turn.h"

namespace {
// View-switch and menu tones: only with [sound] view_cues=1 (off by default since 2026-10-09).
void ViewCue(vrmod::Cue c) {
    if (vrmod::GetViewCuesSetting()) vrmod::PlayCue(c);
}
}  // namespace

extern "C" {
    // Referenced directly by camera_interceptor.asm.
    void* g_cameraStructAddress = nullptr;
    void* g_cameraOwnerAddress = nullptr;   // RCX at the read hook: the gameplay camera object (docs/AIM.md)
    void* g_cameraStructTrampoline = nullptr;
    unsigned char g_cameraOverrideEnabled = 0;
    void* g_cameraWriteContinueAddress = nullptr;

    // Filled by cameraWriteInterceptor just before it replays the game's write (only for our struct):
    // [0..2] position, [3] unused, [4..7] quaternion, [8] FOV. Read by ApplyCameraHeadTracking on the same thread.
    alignas(16) float g_preWriteSnap[12] = {};

    // Writes by the same game copy to a struct other than ours (incremented in the asm; instrumentation only).
    volatile unsigned long long g_cameraOtherWrites = 0;

    // The detours themselves, defined in camera_interceptor.asm.
    void cameraStructInterceptor();
    void cameraWriteInterceptor();

    // Called from cameraWriteInterceptor right after the game's own camera
    // write, with the camera struct pointer in RCX. Defined at the bottom.
    void ApplyCameraHeadTracking(void* camera);

    // A2 (docs/AIM.md): called by cameraWriteInterceptor for records other than ours while this is set (Ctrl+Alt+A).
    unsigned char g_aimInjectEnabled = 0;
    void ApplyAimInjection(void* dst, void* src, unsigned long long returnAddress);
}

namespace vrmod {

namespace {

std::atomic<bool> g_headTrackingRestoreAfterMenu{false};
std::atomic<bool> g_menuScreenActive{false};
float g_structSnapshot[64] = {};               // camera struct floats from +0x80 (position) to +0x17C
std::atomic<bool> g_structSnapshotReady{false};
std::atomic<bool> g_structSnapshotLogged{false};

constexpr uintptr_t kPositionOffset = 0x80;
constexpr uintptr_t kQuaternionOffset = 0xA0;
constexpr uintptr_t kFovOffset = 0xB0;

void ReadPose(uintptr_t base, CamPose* out) {
    const float* p = reinterpret_cast<const float*>(base + kPositionOffset);
    const float* q = reinterpret_cast<const float*>(base + kQuaternionOffset);
    for (int k = 0; k < 3; ++k) out->p[k] = p[k];
    for (int k = 0; k < 4; ++k) out->q[k] = q[k];
    out->fov = *reinterpret_cast<const float*>(base + kFovOffset);
}

// Present thread: the struct can be freed/replaced by the game at any time, so read it under SEH.
bool ReadPoseGuarded(uintptr_t base, CamPose* out) {
    __try {
        ReadPose(base, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct TraceEntry {
    LONGLONG qpc;
    unsigned long long frame;
    float pos[3];
    float quat[4];
    float fov;
};
constexpr int kTraceMax = 128;
TraceEntry g_trace[kTraceMax];
std::atomic<int> g_traceCount{0};
std::atomic<bool> g_traceArmed{false};
std::atomic<bool> g_traceNeedsFlush{false};

// Game frame (detected from write timing) of the latest write to our camera struct; read by the pairing test.
std::atomic<uint64_t> g_latestGameFrame{0};
std::atomic<uint64_t> g_splitMerges{0};
std::atomic<uint64_t> g_foreignWrites{0};
std::atomic<float> g_gameQuat[4];   // the game's own camera rotation of the latest write (before the head delta)
std::atomic<bool> g_nearLayoutBad{false};   // the near/far pair before the struct didn't look right (logged once)
std::atomic<uint64_t> g_adjustedWrites{0};
std::atomic<uint32_t> g_adjustMaxMm{0};   // largest adjustment since the last CameraTakeAdjustMaxMm, in mm

// A1 (docs/AIM.md): the struct is one 0x1A0-byte camera record (FUN_140164300 copies it whole). Besides position,
// rotation and FOV at +0x80.. it holds a camera->world matrix at +0x00 (row-vector convention: rows 0-2 the camera
// axes, row 3 the position) and its inverse at +0x40 -- copied BEFORE the block we hook, so they keep the game's own
// pose while the renderer uses ours. Whatever reads the matrices (the aim, we hope) never saw the head. Ctrl+Alt+A.
std::atomic<uint64_t> g_shakeWrites{0};   // rotation-only changes after our write (recoil, shake): not steering
std::atomic<uint32_t> g_winRotCdeg{0}, g_winPosMm{0};   // largest change in the current 250 ms steering window
void StoreMax(std::atomic<uint32_t>& a, uint32_t v) {
    uint32_t prev = a.load(std::memory_order_relaxed);
    while (v > prev && !a.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {}
}
std::atomic<bool> g_aimSync{false};

// D2: the final camera per game frame (game thread writes, Present / render threads read).
std::mutex g_camFrameMutex;
CamFrameRec g_camFrames[16] = {};
void StoreCamFrame(const CamFrameRec& r) {
    std::lock_guard<std::mutex> lock(g_camFrameMutex);
    g_camFrames[r.frame % 16] = r;
}   // A1 result: all checks passed, the aim didn't follow -- off
std::atomic<uint64_t> g_aimSyncOk{0};
std::atomic<uint64_t> g_aimSyncFail[4] = {};   // axes not orthonormal, row 3 != position, rows != game rotation, +0x40 not the inverse
std::atomic<int> g_aimMap[3] = { { -1 }, { -1 }, { -1 } };   // per row: game axis * 2 + (negated), for the log

// Camera axes in world space (local X right, Y forward, Z up) of a local->world quaternion.
void QuatAxes(const float q[4], float ax[3][3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    ax[0][0] = 1.0f - 2.0f * (y * y + z * z); ax[0][1] = 2.0f * (x * y + w * z);        ax[0][2] = 2.0f * (x * z - w * y);
    ax[1][0] = 2.0f * (x * y - w * z);        ax[1][1] = 1.0f - 2.0f * (x * x + z * z); ax[1][2] = 2.0f * (y * z + w * x);
    ax[2][0] = 2.0f * (x * z + w * y);        ax[2][1] = 2.0f * (y * z - w * x);        ax[2][2] = 1.0f - 2.0f * (x * x + y * y);
}

// Rigid inverse of a row-vector camera->world matrix: [R 0; t 1]^-1 = [R^T 0; -t R^T 1].
void RigidInverse(const float W[16], float out[16]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = W[j * 4 + i];
        out[i * 4 + 3] = 0.0f;
    }
    for (int j = 0; j < 3; ++j) out[12 + j] = -(W[12] * W[j * 4] + W[13] * W[j * 4 + 1] + W[14] * W[j * 4 + 2]);
    out[15] = 1.0f;
}

// Game thread, after our pose is final: rotate the matrices' axes by the same world-space rotation that took the
// game's quaternion to ours, and put our position in row 3. Every precondition is checked on every write; if one
// fails nothing is changed (counted).
void SyncRecordMatrices(uintptr_t base, const CamPose& game) {
    float* W = reinterpret_cast<float*>(base);
    float* M = W + 16;
    const float* pos = reinterpret_cast<const float*>(base + kPositionOffset);
    const float* qF = reinterpret_cast<const float*>(base + kQuaternionOffset);
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    for (int r = 0; r < 3; ++r) {
        if (std::fabs(dot(W + r * 4, W + r * 4) - 1.0f) > 0.02f || std::fabs(W[r * 4 + 3]) > 1e-3f) { g_aimSyncFail[0].fetch_add(1); return; }
        for (int c = r + 1; c < 3; ++c) if (std::fabs(dot(W + r * 4, W + c * 4)) > 0.01f) { g_aimSyncFail[0].fetch_add(1); return; }
    }
    if (std::fabs(W[15] - 1.0f) > 1e-3f) { g_aimSyncFail[0].fetch_add(1); return; }
    for (int k = 0; k < 3; ++k) if (std::fabs(W[12 + k] - game.p[k]) > 1.0f) { g_aimSyncFail[1].fetch_add(1); return; }
    float ax[3][3];
    QuatAxes(game.q, ax);
    for (int r = 0; r < 3; ++r) {
        int found = -1;
        for (int a = 0; a < 3 && found < 0; ++a) {
            const float d = dot(W + r * 4, ax[a]);
            if (std::fabs(d) > 0.999f) found = a * 2 + (d < 0.0f ? 1 : 0);
        }
        if (found < 0) { g_aimSyncFail[2].fetch_add(1); return; }
        g_aimMap[r].store(found, std::memory_order_relaxed);
    }
    float inv[16];
    RigidInverse(W, inv);
    bool invOk = true;
    for (int i = 0; i < 16 && invOk; ++i) invOk = std::fabs(inv[i] - M[i]) <= (i >= 12 ? 0.05f : 1e-3f);

    // d = qF * conj(qG): the world-space rotation from the game's orientation to ours.
    const float gx = -game.q[0], gy = -game.q[1], gz = -game.q[2], gw = game.q[3];
    const float fx = qF[0], fy = qF[1], fz = qF[2], fw = qF[3];
    const float dx = fw * gx + fx * gw + fy * gz - fz * gy;
    const float dy = fw * gy - fx * gz + fy * gw + fz * gx;
    const float dz = fw * gz + fx * gy - fy * gx + fz * gw;
    const float dw = fw * gw - fx * gx - fy * gy - fz * gz;
    float nW[16];
    for (int r = 0; r < 3; ++r) {
        // v' = v + 2w (u x v) + 2 u x (u x v), u = (dx, dy, dz)
        const float* v = W + r * 4;
        const float cx = dy * v[2] - dz * v[1], cy = dz * v[0] - dx * v[2], cz = dx * v[1] - dy * v[0];
        nW[r * 4 + 0] = v[0] + 2.0f * (dw * cx + dy * cz - dz * cy);
        nW[r * 4 + 1] = v[1] + 2.0f * (dw * cy + dz * cx - dx * cz);
        nW[r * 4 + 2] = v[2] + 2.0f * (dw * cz + dx * cy - dy * cx);
        nW[r * 4 + 3] = W[r * 4 + 3];
    }
    for (int k = 0; k < 3; ++k) nW[12 + k] = pos[k];
    nW[15] = W[15];
    for (int i = 0; i < 16; ++i) W[i] = nW[i];
    if (invOk) {
        RigidInverse(nW, inv);
        for (int i = 0; i < 16; ++i) M[i] = inv[i];
        g_aimSyncOk.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_aimSyncFail[3].fetch_add(1, std::memory_order_relaxed);
    }
}

// A2: the camera component's record (+0x2B0) is written by the copy called from its blend stack FUN_140167df0; the
// call returns to 0x140168233 (verified at install). Ctrl+Alt+A turns a fixed yaw offset on there, to see whether the
// view (downstream) and the aim follow it.
// A2b: the component's four current-frame records -- first blend stack FUN_140167df0: +0x450 (before the +0xC78
// post-process) and +0x2B0 (after); second stack FUN_140168270: +0x930 and +0x790. Return addresses of the copies
// (Ghidra RVAs). The previous-frame copies (+0x5F0, +0xAD0) and the director's slot copies copy these, so they are
// left alone (they'd double the offset).
const GameAddr kAimInjectRet[4] = { GameAddr::AimRet0, GameAddr::AimRet1, GameAddr::AimRet2, GameAddr::AimRet3 };   // by pattern
const char* const kAimInjectName[4] = { "+0x2B0", "+0x450", "+0x930", "+0x790" };
uintptr_t g_aimInjectRets[4] = {};
uintptr_t g_aimInjectRet = 0;   // nonzero once at least one call site checked out
constexpr float kAimInjectDeg = 20.0f;
std::atomic<uint64_t> g_aimInjectHits{0}, g_aimInjectMatrixSkips{0};
struct CallerSlot { std::atomic<uint64_t> ret{0}; std::atomic<uint64_t> count{0}; };
CallerSlot g_aimCallers[32];

void RecordAimCaller(uint64_t ret) {
    for (CallerSlot& c : g_aimCallers) {
        uint64_t cur = c.ret.load(std::memory_order_relaxed);
        if (cur == 0) {
            uint64_t expected = 0;
            cur = c.ret.compare_exchange_strong(expected, ret) ? ret : expected;
        }
        if (cur == ret) { c.count.fetch_add(1, std::memory_order_relaxed); return; }
    }
}

// A2c: 1 = the component's records (Ctrl+Alt+A; the aim follows, but each aim press bakes the offset into the game's
// state: aim modes start from the component's last output), 2 = only the director's copy of +0x450 into our slot
// (the record 0x1A0 before our struct; K) -- read by the aim, hopefully not by the controllers.
std::atomic<int> g_aimInjectMode{0};
std::atomic<uint64_t> g_slotAimRet{0}, g_slotAimWrites{0};
// A2d: mode 2 found no write to struct - 0x1A0 (19:39 run). Instead: the component's aim records are learned from
// the blend stacks' own copies (+0x450 at 0x140168213, +0x930 at 0x1401687A3), and every OTHER copy whose source is
// one of them (the director's copies into its slots) is injected -- except the component's previous-frame copies
// (+0x5F0 <- +0x450 at 0x140167E42, +0xAD0 <- +0x930 at 0x1401682DA).
std::atomic<uintptr_t> g_comp450{0}, g_comp930{0};
uintptr_t g_prevCopyRet[2] = {};
CallerSlot g_slotInjectCallers[8];

// Rotates one camera record by a world-space rotation dw: quaternion q' = dw * q, the matrices' axis rows rotated the
// same way (if the layout checks out) and +0x40 recomputed as the inverse, and the -yaw copy (+0xBC, copied after
// this block from the source) set in the source when that is a stack temporary (src != 0).
void RotateRecord(uintptr_t dst, uintptr_t src, const float dw[4]) {
    float* q = reinterpret_cast<float*>(dst + kQuaternionOffset);
    const float ax_ = dw[0], ay_ = dw[1], az_ = dw[2], aw_ = dw[3];
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = aw_ * x + ax_ * w + ay_ * z - az_ * y;
    q[1] = aw_ * y - ax_ * z + ay_ * w + az_ * x;
    q[2] = aw_ * z + ax_ * y - ay_ * x + az_ * w;
    q[3] = aw_ * w - ax_ * x - ay_ * y - az_ * z;
    float* W = reinterpret_cast<float*>(dst);
    float* M = W + 16;
    const float* pos = reinterpret_cast<const float*>(dst + kPositionOffset);
    bool ok = std::fabs(W[15] - 1.0f) < 1e-3f;
    for (int r = 0; r < 3 && ok; ++r) {
        const float l = W[r * 4] * W[r * 4] + W[r * 4 + 1] * W[r * 4 + 1] + W[r * 4 + 2] * W[r * 4 + 2];
        ok = std::fabs(l - 1.0f) < 0.02f && std::fabs(W[r * 4 + 3]) < 1e-3f;
    }
    for (int k = 0; k < 3 && ok; ++k) ok = std::fabs(W[12 + k] - pos[k]) < 1.0f;
    if (!ok) {
        g_aimInjectMatrixSkips.fetch_add(1, std::memory_order_relaxed);
    } else {
        float inv[16];
        RigidInverse(W, inv);
        bool invOk = true;
        for (int i = 0; i < 16 && invOk; ++i) invOk = std::fabs(inv[i] - M[i]) <= (i >= 12 ? 0.05f : 1e-3f);
        for (int r = 0; r < 3; ++r) {
            // v' = v + 2w (u x v) + 2 u x (u x v), u = (ax_, ay_, az_)
            float* v = W + r * 4;
            const float cx = ay_ * v[2] - az_ * v[1], cy = az_ * v[0] - ax_ * v[2], cz = ax_ * v[1] - ay_ * v[0];
            const float nx = v[0] + 2.0f * (aw_ * cx + ay_ * cz - az_ * cy);
            const float ny = v[1] + 2.0f * (aw_ * cy + az_ * cx - ax_ * cz);
            const float nz = v[2] + 2.0f * (aw_ * cz + ax_ * cy - ay_ * cx);
            v[0] = nx; v[1] = ny; v[2] = nz;
        }
        if (invOk) {
            RigidInverse(W, inv);
            for (int i = 0; i < 16; ++i) M[i] = inv[i];
        }
    }
    if (src) {
        float ax[3][3];
        QuatAxes(q, ax);
        *reinterpret_cast<float*>(src + 0xBC) = -std::atan2(ax[1][0], ax[1][1]);
    }
    g_aimInjectHits.fetch_add(1, std::memory_order_relaxed);
}

void InjectYaw(uintptr_t dst, uintptr_t src) {
    const float th = kAimInjectDeg / 57.29578f;
    const float dw[4] = { 0.0f, 0.0f, std::sin(th * 0.5f), std::cos(th * 0.5f) };   // about world Z
    RotateRecord(dst, src, dw);
}

// A3 (docs/AIM.md): aim where you look. The aim follows the camera component's records (A2b), so the head's yaw and
// pitch (AerHeadAimDelta, camera-local d) go into all four of them each frame -- q' = q * d -- and our slot hook takes
// exactly that d off again before composing the eye pose as before, so the view is unchanged while the game's aim,
// Lara and the crosshair follow the head. K toggles it (AER only); suspended in menus, cutscenes and third-person
// moments.
std::atomic<bool> g_headAim{false};
std::atomic<bool> g_headAimSuspended{false};
std::atomic<bool> g_hideHeadOn{true};   // [firstperson] hide_head; Ctrl+Alt+F5 (head, bow when not aiming, quiver)
std::atomic<uint64_t> g_headAimInjected{0}, g_headAimUndone{0}, g_headAimUndoMatched{0}, g_headAimNoDelta{0};
// Game thread only (the blend stacks and our slot write run on the camera update thread):
float g_aimD[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // the d injected in the current game frame
uint64_t g_aimDKey = ~0ull;                      // g_latestGameFrame when g_aimD was taken (the previous slot frame)
float g_aimInjQ[2][4] = {};                      // injected +0x2B0 / +0x790 quaternions of this frame
bool g_aimInjValid[2] = { false, false };

// A3b: parallax. In first person we look from Lara's head, but the game aims from its own (third-person) camera
// position, so shots land beside the crosshair (2026-09-26: "a little to the left"). The eye's offset from the game
// camera, in the game camera's axes, is taken at each slot write; the aim records (+0x450, +0x930 -- not the view
// records, whose position the first-person code still derives its head hint from) are moved to the eye.
float g_eyeOffLocal[3] = {};
uint64_t g_eyeOffFrame = 0;   // slot frame of g_eyeOffLocal; 0 = none
std::atomic<float> g_eyeOffLog[3];   // latest offset for the log (right, forward, up; cm)
std::atomic<uint64_t> g_headAimMoved{0};

void QuatRotate(const float q[4], const float v[3], float out[3]) {   // q v q*
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float cx = y * v[2] - z * v[1], cy = z * v[0] - x * v[2], cz = x * v[1] - y * v[0];
    out[0] = v[0] + 2.0f * (w * cx + y * cz - z * cy);
    out[1] = v[1] + 2.0f * (w * cy + z * cx - x * cz);
    out[2] = v[2] + 2.0f * (w * cz + x * cy - y * cx);
}

// Moves a record's position (+0x80 and the matrix row 3), keeping +0x40 the inverse when it was.
void SetRecordPosition(uintptr_t dst, const float p[3]) {
    float* pos = reinterpret_cast<float*>(dst + kPositionOffset);
    float* W = reinterpret_cast<float*>(dst);
    float* M = W + 16;
    bool rowOk = true;
    for (int k = 0; k < 3; ++k) rowOk = rowOk && std::fabs(W[12 + k] - pos[k]) < 1.0f;
    float inv[16];
    RigidInverse(W, inv);
    bool invOk = true;
    for (int i = 0; i < 16 && invOk; ++i) invOk = std::fabs(inv[i] - M[i]) <= (i >= 12 ? 0.05f : 1e-3f);
    for (int k = 0; k < 3; ++k) pos[k] = p[k];
    if (rowOk) {
        for (int k = 0; k < 3; ++k) W[12 + k] = p[k];
        if (invOk) {
            RigidInverse(W, inv);
            for (int i = 0; i < 16; ++i) M[i] = inv[i];
        }
    }
}

// A3b parallax alone: the aim records (+0x450 = which 1, +0x930 = which 2) moved from the game camera to the eye. Also
// under head drive (2026-10-09: head drive stood the whole injection down, so the targeting ray started at the
// third-person camera again -- 3.45 m behind and ~24 cm beside the eye -- and red / rope targeting needed the crosshair
// held off the target, more so up close).
bool HeadAimMoveToEye(int which, uintptr_t dst) {
    const uint64_t key = g_latestGameFrame.load(std::memory_order_relaxed);
    if (!(which == 1 || which == 2) || !IsFirstPersonEnabled() || g_eyeOffFrame == 0 || key + 1 < g_eyeOffFrame || key > g_eyeOffFrame + 2) return false;
    const float* q = reinterpret_cast<const float*>(dst + kQuaternionOffset);
    const float q0[4] = { q[0], q[1], q[2], q[3] };
    float w[3];
    QuatRotate(q0, g_eyeOffLocal, w);
    const float* pos = reinterpret_cast<const float*>(dst + kPositionOffset);
    const float np[3] = { pos[0] + w[0], pos[1] + w[1], pos[2] + w[2] };
    SetRecordPosition(dst, np);
    g_headAimMoved.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void HeadAimInject(int which, uintptr_t dst, uintptr_t src) {
    const uint64_t key = g_latestGameFrame.load(std::memory_order_relaxed);
    if (key != g_aimDKey) {
        g_aimDKey = key;
        g_aimInjValid[0] = g_aimInjValid[1] = false;
        if (!AerHeadAimDelta(g_aimD)) {
            g_aimD[0] = g_aimD[1] = g_aimD[2] = 0.0f;
            g_aimD[3] = 1.0f;
            g_headAimNoDelta.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Rope-shot camera: it already points where the head aimed at the charge (head drive's hold), so only the
            // head's change since then goes into the aim: d' = h0^-1 * d.
            float h0[4];
            if (HeadDriveRopeHead(h0)) {
                const float c[4] = { -h0[0], -h0[1], -h0[2], h0[3] };
                float o[4];
                const float* a = c;
                const float* b = g_aimD;
                o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
                o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
                o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
                o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
                for (int k = 0; k < 4; ++k) g_aimD[k] = o[k];
            }
        }
    }
    const float* q = reinterpret_cast<const float*>(dst + kQuaternionOffset);
    // Parallax (aim records only, first person): the eye, from the game camera's own (un-turned) rotation.
    HeadAimMoveToEye(which, dst);
    // World rotation for q * d: dw = q d q*.
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float* d = g_aimD;
    const float tx = qw * d[0] + qx * d[3] + qy * d[2] - qz * d[1];   // t = q * d
    const float ty = qw * d[1] - qx * d[2] + qy * d[3] + qz * d[0];
    const float tz = qw * d[2] + qx * d[1] - qy * d[0] + qz * d[3];
    const float tw = qw * d[3] - qx * d[0] - qy * d[1] - qz * d[2];
    const float dw[4] = { tw * -qx + tx * qw + ty * -qz - tz * -qy,  // dw = t * conj(q)
                          tw * -qy - tx * -qz + ty * qw + tz * -qx,
                          tw * -qz + tx * -qy - ty * -qx + tz * qw,
                          tw * qw - tx * -qx - ty * -qy - tz * -qz };
    RotateRecord(dst, src, dw);
    if (which == 0 || which == 3) {
        const int k = which == 0 ? 0 : 1;
        for (int c = 0; c < 4; ++c) g_aimInjQ[k][c] = q[c];
        g_aimInjValid[k] = true;
    }
    g_headAimInjected.fetch_add(1, std::memory_order_relaxed);
}

// Slot hook, before anything else uses the rotation: take this frame's injected d off again (q * conj(d)) when the
// component's view records were injected for this frame. `gameFrame` is the slot's frame; injection saw the previous.
void HeadAimUndo(float* quat, uint64_t gameFrame) {
    if (!g_headAim.load(std::memory_order_relaxed) || g_aimDKey + 1 != gameFrame || !(g_aimInjValid[0] || g_aimInjValid[1])) return;
    bool matched = false;
    for (int k = 0; k < 2; ++k) {
        if (!g_aimInjValid[k]) continue;
        float dot = 0.0f;
        for (int c = 0; c < 4; ++c) dot += quat[c] * g_aimInjQ[k][c];
        if (std::fabs(dot) > 0.99999f) matched = true;
    }
    const float x = quat[0], y = quat[1], z = quat[2], w = quat[3];
    const float bx = -g_aimD[0], by = -g_aimD[1], bz = -g_aimD[2], bw = g_aimD[3];
    quat[0] = w * bx + x * bw + y * bz - z * by;
    quat[1] = w * by - x * bz + y * bw + z * bx;
    quat[2] = w * bz + x * by - y * bx + z * bw;
    quat[3] = w * bw - x * bx - y * by - z * bz;
    g_headAimUndone.fetch_add(1, std::memory_order_relaxed);
    if (matched) g_headAimUndoMatched.fetch_add(1, std::memory_order_relaxed);
}

// Camera modes (2026-09-26): the component's blend stacks run one controller per camera mode (entry +0x20; RTTI names
// in the exe: ExplorationCameraController, AimCameraController, StealthCameraController, SplineCameraController,
// CameraBoneCameraController, AnchoredCameraController, ForceCameraController, ...). Entries used in this frame's
// blend have +0x1C = 1 (FUN_140167df0 / FUN_140168270). Logged whenever the set changes -- a direct signal of a
// game-directed camera, instead of inferring it from adjustments to our writes.
std::atomic<uintptr_t> g_cameraComponent{0};

struct ModeEnt { uint64_t vt; float w0, w1, t; uint8_t used; uint8_t list; };

// Raw reads only (SEH; the game thread may be changing the lists).
int ReadModeEntries(uintptr_t c, ModeEnt* out, int max) {
    static const uint16_t kLists[7][2] = { { 0xF00, 0xF08 }, { 0xF18, 0xF20 }, { 0xF30, 0xF38 }, { 0xF48, 0xF50 },
                                           { 0xF60, 0xF68 }, { 0xF78, 0xF80 }, { 0xF90, 0xF98 } };
    int n = 0;
    __try {
        auto add = [&](uintptr_t e, int list) {
            if (n >= max) return;
            const float* f = reinterpret_cast<const float*>(e);
            const uintptr_t ctrl = *reinterpret_cast<const uintptr_t*>(e + 0x20);
            out[n].vt = ctrl ? *reinterpret_cast<const uint64_t*>(ctrl) : 0;
            out[n].w0 = f[0]; out[n].w1 = f[1]; out[n].t = f[3];
            out[n].used = *reinterpret_cast<const uint8_t*>(e + 0x1C);
            out[n].list = static_cast<uint8_t>(list);
            ++n;
        };
        add(c + 0xD20, 0);
        for (int l = 0; l < 7; ++l) {
            const uint32_t count = *reinterpret_cast<const uint32_t*>(c + kLists[l][0]);
            const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(c + kLists[l][1]);
            if (!arr) continue;
            for (uint32_t i = 0; i < count && i < 16; ++i) add(arr + i * 0x1E0, l + 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

// MSVC x64 RTTI: vtable[-1] -> complete object locator (signature 1; +0x0C type descriptor RVA); name at +0x10.
bool RttiNameRaw(uint64_t vt, char* out, size_t cap) {
    const uintptr_t mod = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    __try {
        const uintptr_t col = *reinterpret_cast<const uintptr_t*>(vt - 8);
        if (*reinterpret_cast<const uint32_t*>(col) != 1) return false;
        const char* name = reinterpret_cast<const char*>(mod + *reinterpret_cast<const uint32_t*>(col + 0x0C) + 0x10);
        size_t i = 0;
        for (; i + 1 < cap && name[i]; ++i) out[i] = name[i];
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float ReadFloatGuarded(uintptr_t a) {
    __try { return *reinterpret_cast<const float*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}

std::string ModeName(uint64_t vt) {
    if (!vt) return "(none)";
    char buf[128];
    if (RttiNameRaw(vt, buf, sizeof buf)) {
        std::string n = buf;
        if (n.rfind(".?AV", 0) == 0) n = n.substr(4);
        const size_t at = n.find("@@");
        if (at != std::string::npos) n = n.substr(0, at);
        const size_t cc = n.find("CameraController");
        if (cc != std::string::npos && cc > 0) n = n.substr(0, cc);
        return n;
    }
    const uintptr_t mod = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::ostringstream o;
    o << "vt 0x" << std::hex << (vt - mod + 0x140000000ull);
    return o.str();
}

// Which camera classes mean the game is directing the camera (2026-09-26 run: cutscenes = Spline / Simple / Anchored
// base or a Dummy overlay; interactions and some animations = CameraBone; the opening's scripted walk = Anchored).
// Exploration, Aim and Stealth are player-driven. Cached per vtable.
std::atomic<bool> g_aimingNow{false};
std::atomic<uint64_t> g_playerCameraFrames{0};
std::atomic<bool> g_ropeShot{false};   // game frames with a player-controlled base camera (see CamModeWatch)

bool AimClass(uint64_t vt) {
    static std::unordered_map<uint64_t, bool> cache;
    const auto it = cache.find(vt);
    if (it != cache.end()) return it->second;
    const bool aim = ModeName(vt).rfind("Aim", 0) == 0;
    cache.emplace(vt, aim);
    return aim;
}

bool PlayerClass(uint64_t vt) {
    static std::unordered_map<uint64_t, bool> cache;
    const auto it = cache.find(vt);
    if (it != cache.end()) return it->second;
    const std::string m = ModeName(vt);
    const bool player = m.rfind("Exploration", 0) == 0 || m.rfind("Aim", 0) == 0 || m.rfind("Stealth", 0) == 0;
    cache.emplace(vt, player);
    return player;
}

bool ScriptedClass(uint64_t vt) {
    static std::unordered_map<uint64_t, bool> cache;
    auto it = cache.find(vt);
    if (it != cache.end()) return it->second;
    const std::string n = ModeName(vt);
    const bool scripted = n == "Spline" || n == "Simple" || n == "Anchored" || n == "CameraBone" || n == "Dummy" || n == "Force";
    cache.emplace(vt, scripted);
    return scripted;
}

void CamModeWatch() {
    const uintptr_t c = g_cameraComponent.load(std::memory_order_relaxed);
    if (!c) return;
    ModeEnt e[64];
    const int n = ReadModeEntries(c, e, 64);
    if (n <= 0) return;
    // Third person while a scripted camera is in use: as the base entry, or in stack 2 while stack 2 dominates.
    {
        const float w2now = ReadFloatGuarded(c + 0xC70);
        bool scripted = false;
        for (int i = 0; i < n && !scripted; ++i) {
            if (!e[i].used || !e[i].vt) continue;
            if (e[i].list == 0 || (e[i].list >= 3 && w2now > 0.5f)) scripted = ScriptedClass(e[i].vt);
        }
        const double now = NowMs();
        bool aiming = false;
        for (int i = 0; i < n && !aiming; ++i) aiming = e[i].used && e[i].vt && AimClass(e[i].vt);
        // Rope-shot camera (2026-10-09): charging a rope arrow swaps the aim camera for a Simple base camera (and a few
        // unnamed ones) until the walking camera returns -- taken for a cutscene, it flipped to third person and back
        // while the user lined the shot up. A scripted camera within 1.5 s of the aim camera continues the aim: first
        // person stays, and it counts as aiming (bow shown). Real cutscenes are still caught by their events.
        static double lastAimMs = 0.0, aimContSince = 0.0;
        static bool aimCont = false;
        if (aiming) {
            lastAimMs = now;
            aimCont = false;
        } else {
            bool basePlayer = false;
            for (int i = 0; i < n && !basePlayer; ++i) basePlayer = e[i].used && e[i].vt && e[i].list == 0 && PlayerClass(e[i].vt);
            if (!aimCont && scripted && now - lastAimMs < 1500.0) {
                aimCont = true;
                aimContSince = now;
                modlog::Line("CAMMODE: rope-shot camera after aiming -- first person kept, counted as aiming");
            } else if (aimCont && (basePlayer || now - aimContSince > 20000.0)) {
                aimCont = false;
                modlog::Line(std::string("CAMMODE: rope-shot camera over (") + (basePlayer ? "walking camera back" : "20 s") + ")");
            }
        }
        if (aimCont) {
            scripted = false;
            aiming = true;
        }
        g_ropeShot.store(aimCont, std::memory_order_relaxed);
        // Debounced: only a scripted camera held for 0.4 s counts. In some places (seen 2026-10-07 after turning on
        // ray-traced shadows) the game flickers its base camera to Simple for a moment every few seconds, which flipped
        // first person off and on; real cutscenes hold theirs for seconds.
        static double scriptedSince = 0.0;
        static int flickers = 0;
        static double flickerLogMs = 0.0;
        if (scripted) {
            if (scriptedSince == 0.0) scriptedSince = now;
            if (now - scriptedSince >= 400.0) CameraReasonPulse(ThirdPersonReason::GameCamera, 0.5);
        } else if (scriptedSince != 0.0) {
            if (now - scriptedSince < 400.0) ++flickers;
            scriptedSince = 0.0;
        }
        if (flickers && now - flickerLogMs > 30000.0) {
            modlog::Line("CAMMODE: " + std::to_string(flickers) + " scripted-camera flicker(s) under 0.4 s ignored (first person kept)");
            flickers = 0;
            flickerLogMs = now;
        }
        g_aimingNow.store(aiming, std::memory_order_relaxed);
        // Player-controlled camera in use (exploration / aim / stealth, nothing scripted): counted, so the
        // intro detection can tell a new game (straight into cutscenes) from a loaded save (gameplay first).
        // Any used entry, not only the base one: in ordinary play the walking camera is usually in stack 1 with the base
        // entry unused (2026-10-09: 28 s of play after loading a save counted nothing, and the next cutscene was taken for
        // a new game's intro -- flat, head tracking off).
        if (!scripted) {
            for (int i = 0; i < n; ++i) {
                if (!e[i].used || !e[i].vt) continue;
                if (PlayerClass(e[i].vt)) {
                    g_playerCameraFrames.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }
    std::string sig;
    for (int i = 0; i < n; ++i) {
        sig += std::to_string(e[i].list) + ':' + std::to_string(e[i].vt) + (e[i].used ? "*" : "") + ' ';
    }
    static std::string lastSig;
    static int lines = 0;
    static double lastMs = 0.0;
    if (sig == lastSig || lines >= 3000 || NowMs() - lastMs < 100.0) return;
    lastSig = sig;
    lastMs = NowMs();
    ++lines;
    const float w2 = ReadFloatGuarded(c + 0xC70);
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "CAMMODE:";
    int list = -1;
    for (int i = 0; i < n; ++i) {
        if (e[i].list != list) {
            list = e[i].list;
            o << (list == 0 ? " base[" : list <= 2 ? " | stack1." : " | stack2.");
            if (list > 0) o << (list <= 2 ? list : list - 2) << "[";
        } else {
            o << ",";
        }
        o << ModeName(e[i].vt) << (e[i].used ? "*" : "");
        if (i + 1 == n || e[i + 1].list != list) o << "]";
    }
    o << " | stack2 weight " << w2 << (IsFirstPersonEnabled() ? " (first person)" : " (third person)");
    modlog::Line(o.str());
}

// Head aim on/off (K, or gamepad LB + right stick click held 0.6 s).
void ToggleHeadAim(const char* how) {
    if (!g_aimInjectRet) {
        modlog::Line("HEADAIM: not available (the blend stacks' record copies weren't where expected).");
        PlayCue(Cue::Failure);
    } else if (GetRenderMode() != RenderMode::Aer) {
        modlog::Line("HEADAIM: needs render mode aer.");
        PlayCue(Cue::Failure);
    } else {
        const bool on = !g_headAim.load();
        g_aimInjectMode.store(0);
        g_headAim.store(on);
        SetHeadAimForSave(on);
        modlog::Line(std::string("HEADAIM: aim where you look ") + (on ? "ON" : "off") + " (" + how + "; Ctrl+Alt+0 saves)");
        PlayCue(on ? Cue::On : Cue::Off);
    }
}

// Present thread: Ctrl+Alt+A toggles the A2 yaw injection; A1's layout once and the counts every 30 s go to the log.
void PollAimSync() {
    // K alone as well (no modifiers): with a gamepad in hand (aim held on the trigger) a three-key chord is impractical,
    // A is a movement key, and not every keyboard has Scroll Lock. K has no default binding in the game.
    static bool aWas = false, scrollWas = false;
    const bool a = (VRMOD_DEV_KEY('A') & 0x8000) != 0;
    const bool scroll = (GetAsyncKeyState('K') & 0x8000) != 0 && !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool scrollPressed = scroll && !scrollWas;
    scrollWas = scroll;
    if (scrollPressed) ToggleHeadAim("K");
    const int want = (a && !aWas && ca) ? 1 : 0;
    if (want == 1 && !g_aimInjectRet) {
        modlog::Line("AIMINJECT: not available (the blend stack's call to the record copy wasn't where expected).");
        PlayCue(Cue::Failure);
    } else if (want) {
        const int mode = g_aimInjectMode.load() == want ? 0 : want;
        g_headAim.store(false);
        g_aimInjectMode.store(mode);
        modlog::Line(std::string("AIMINJECT: +") + std::to_string(static_cast<int>(kAimInjectDeg)) + " deg yaw "
            + (mode == 1 ? "into the camera component's records (+0x2B0, +0x450, +0x930, +0x790) -- Ctrl+Alt+A"
               : mode == 2 ? "into the director's copies of the component's +0x450 / +0x930 (not the component itself) -- K" : "off"));
        PlayCue(mode ? Cue::On : Cue::Off);
    }
    aWas = a;
    {
        static double onSinceMs = 0.0;
        static bool callersLogged = false;
        if (!g_aimInjectEnabled) { onSinceMs = 0.0; callersLogged = false; }   // logged again each time a test is turned on
        else if (onSinceMs == 0.0) onSinceMs = NowMs();
        if (!callersLogged && onSinceMs > 0.0 && NowMs() - onSinceMs > 8000.0) {
            callersLogged = true;
            std::ostringstream o;
            o << "AIMINJECT: record copies to other structs by caller (return address, Ghidra):";
            const uintptr_t mod = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            for (const CallerSlot& c : g_aimCallers) {
                const uint64_t r = c.ret.load();
                if (!r) continue;
                o << " 0x" << std::hex << (r - mod + 0x140000000ull) << std::dec << " x" << c.count.load();
                for (int k = 0; k < 4; ++k) if (r == g_aimInjectRets[k]) o << " (component " << kAimInjectName[k] << ": injected)";
            }
            o << " | injected " << g_aimInjectHits.load() << ", matrices left alone " << g_aimInjectMatrixSkips.load()
              << " | copies from the component's aim records injected (mode K):";
            for (const CallerSlot& c : g_slotInjectCallers) {
                if (!c.ret.load()) continue;
                o << " 0x" << std::hex << (c.ret.load() - mod + 0x140000000ull) << std::dec << " x" << c.count.load();
            }
            modlog::Line(o.str());
        }
    }
    static bool layoutLogged = false;
    if (!layoutLogged && g_aimMap[0].load() >= 0 && g_aimMap[1].load() >= 0 && g_aimMap[2].load() >= 0) {
        layoutLogged = true;
        static const char* const kAxis[3] = { "X (right)", "Y (forward)", "Z (up)" };
        std::ostringstream o;
        o << "AIMSYNC: record matrix +0x00 rows are the camera's";
        for (int r = 0; r < 3; ++r) {
            const int m = g_aimMap[r].load();
            o << (r ? "," : "") << " " << ((m & 1) ? "-" : "+") << kAxis[m >> 1];
        }
        o << "; row 3 the position.";
        modlog::Line(o.str());
    }
    static double lastMs = 0.0;
    if (NowMs() - lastMs < 30000.0) return;
    lastMs = NowMs();
    {
        const uint64_t inj = g_headAimInjected.exchange(0), und = g_headAimUndone.exchange(0), mat = g_headAimUndoMatched.exchange(0),
                       nod = g_headAimNoDelta.exchange(0);
        if (g_headAim.load() || inj)
            modlog::Line("HEADAIM (30 s, " + std::string(g_headAim.load() ? (g_headAimSuspended.load() ? "on, suspended now" : "on") : "off") + "): "
                + std::to_string(inj) + " records turned, " + std::to_string(und) + " slot writes turned back (" + std::to_string(mat)
                + " exactly the injected record), " + std::to_string(nod) + " frames without head views; aim records moved to the eye "
                + std::to_string(g_headAimMoved.exchange(0)) + " times, eye from the game camera (right, forward, up) "
                + std::to_string(static_cast<int>(g_eyeOffLog[0].load())) + ", " + std::to_string(static_cast<int>(g_eyeOffLog[1].load())) + ", "
                + std::to_string(static_cast<int>(g_eyeOffLog[2].load())) + " cm");
    }
    const uint64_t ok = g_aimSyncOk.exchange(0), f0 = g_aimSyncFail[0].exchange(0), f1 = g_aimSyncFail[1].exchange(0),
                   f2 = g_aimSyncFail[2].exchange(0), f3 = g_aimSyncFail[3].exchange(0);
    if (ok + f0 + f1 + f2 + f3 == 0) return;
    modlog::Line("AIMSYNC (30 s, " + std::string(g_aimSync.load() ? "on" : "off") + "): " + std::to_string(ok) + " records synced; skipped: "
        + std::to_string(f0) + " axes not orthonormal, " + std::to_string(f1) + " row 3 != position, " + std::to_string(f2)
        + " rows != game rotation; " + std::to_string(f3) + " with +0x40 not the inverse (only +0x00 synced)");
}
std::atomic<uint32_t> g_cameraWriteTid{0};   // thread of the latest write (the game's update thread)
std::atomic<bool> g_fovTagging{false};       // see CameraSetFovTagging
std::atomic<int> g_injectStallMs{0};         // see CameraInjectStall

// First-person proof of concept (tuned live with Home / PageUp / PageDown).
std::atomic<bool> g_firstPersonEnabled{false};
std::atomic<float> g_firstPersonDistance{2.0f};   // metres along the game's view direction
std::atomic<float> g_firstPersonHeight{0.0f};     // metres up
std::atomic<float> g_firstPersonSide{0.0f};       // metres to the camera's right (negative = left)
std::atomic<float> g_worldUnitsPerMetre{100.0f};  // the game's world units are centimetres (positions in the tens of thousands, camera speed ~100+ units/s)

// Stopgap for the head-bob problem: Lara's skeletal head bob is not in the camera struct we read/write (the
// camera is a fixed boom, unaffected), so we can't track or hide it directly. Instead, while she is moving fast
// (running/swimming), the camera is pushed a bit further forward than the tuned distance -- outrunning the bob
// rather than removing it. Speed is estimated every frame (not the 1 Hz CAMPOS estimate, too slow to react) from
// the raw boom position, exponentially smoothed. Alt+Home cycles how large the extra push is.
std::atomic<float> g_runPushMetres{0.0f};         // added to distance while running; computed each frame, not saved
// 0.6-0.7 m clears the running bob (confirmed by testing); default to the safer end.
std::atomic<float> g_runPushMaxMetres{0.7f};
constexpr float kRunThresholdMps = 1.2f;   // below this, no extra push (walking)
constexpr float kRunRampMps = 2.0f;        // full push reached this many m/s above the threshold (running/sprint)

// AOB pattern from FransBouma/InjectableGenericCameraSystem's SOTTR camera
// (CAMERA_ADDRESS_INTERCEPT_KEY in InterceptorHelper.cpp). No wildcard bytes
// in this particular pattern, so a plain memcmp scan is enough.
const unsigned char kPattern[] = {
    0x89, 0x59, 0x28, 0x45, 0x33, 0xC0, 0x0F, 0x28, 0x82,
    0x80, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xF9, 0x41, 0x0F, 0xB6, 0xD1
};
constexpr size_t kPatternLength = sizeof(kPattern);

// CAMERA_WRITE1_INTERCEPT_KEY from the reference project. Matches the game's
// own per-frame write of freshly-computed position/rotation/FOV into the
// camera struct -- this is what reverted our first test write within a frame.
const unsigned char kWritePattern[] = {
    0x66, 0x0F, 0x7F, 0x81, 0x80, 0x00, 0x00, 0x00,
    0x0F, 0x28, 0x8A, 0xA0, 0x00, 0x00, 0x00,
    0x0F, 0x29, 0x89, 0xA0, 0x00, 0x00, 0x00,
    0x8B, 0x82
};
constexpr size_t kWritePatternLength = sizeof(kWritePattern);
// Length in bytes of the full original instruction sequence this pattern is
// the start of (verified against the reference project's own hook-length
// constant, 0x22): movdqa + movaps + movaps + mov + mov.
constexpr uintptr_t kWriteBlockLength = 0x22;

bool GetModuleRange(uintptr_t* outBase, size_t* outSize) {
    HMODULE hModule = GetModuleHandleW(nullptr);
    if (!hModule) {
        return false;
    }
    auto base = reinterpret_cast<uintptr_t>(hModule);
    auto dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
    *outBase = base;
    *outSize = ntHeaders->OptionalHeader.SizeOfImage;
    return true;
}

void* ScanForPattern(uintptr_t base, size_t size, const unsigned char* pattern, size_t patternLength) {
    // Scanning executable code, not arbitrary data -- straightforward linear
    // memcmp scan is fine here, this only runs once at startup.
    const unsigned char* start = reinterpret_cast<const unsigned char*>(base);
    for (size_t i = 0; i + patternLength <= size; ++i) {
        if (memcmp(start + i, pattern, patternLength) == 0) {
            return const_cast<unsigned char*>(start + i);
        }
    }
    return nullptr;
}

} // namespace

bool CameraAimingNow() { return g_aimingNow.load(std::memory_order_relaxed); }
uint64_t PlayerCameraFrames() { return g_playerCameraFrames.load(std::memory_order_relaxed); }
bool CameraRopeShot() { return g_ropeShot.load(std::memory_order_relaxed); }

bool InstallCameraInterceptor() {
    uintptr_t moduleBase = 0;
    size_t moduleSize = 0;
    if (!GetModuleRange(&moduleBase, &moduleSize)) {
        modlog::Line("CameraInterceptor: could not get module range.");
        return false;
    }

    void* target = ScanForPattern(moduleBase, moduleSize, kPattern, kPatternLength);
    if (!target) {
        modlog::Line("CameraInterceptor: AOB pattern not found (game version may differ from what the pattern was captured from).");
        return false;
    }

    std::ostringstream oss;
    oss << "CameraInterceptor: pattern found at RVA 0x" << std::hex
        << (reinterpret_cast<uintptr_t>(target) - moduleBase);
    modlog::Line(oss.str());

    // MH_Initialize() already called by InstallPresentHook() before this runs.
    if (MH_CreateHook(target, reinterpret_cast<void*>(&cameraStructInterceptor), &g_cameraStructTrampoline) != MH_OK
        || MH_EnableHook(target) != MH_OK) {
        modlog::Line("CameraInterceptor: hook setup failed.");
        return false;
    }

    modlog::Line("CameraInterceptor: installed. Press F9 in-game to dump position/rotation/FOV.");

    // Second hook: the game's own per-frame write of the struct. It now runs
    // the original writes, then patches head tracking + FOV on top. See
    // camera_interceptor.asm for why this can't just be a MinHook trampoline
    // call like the first hook.
    void* writeTarget = ScanForPattern(moduleBase, moduleSize, kWritePattern, kWritePatternLength);
    if (!writeTarget) {
        modlog::Line("CameraInterceptor: write-block AOB pattern not found. Reads will work but writes won't stick.");
        return true;
    }

    g_cameraWriteContinueAddress = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(writeTarget) + kWriteBlockLength);

    void* unusedTrampoline = nullptr;
    if (MH_CreateHook(writeTarget, reinterpret_cast<void*>(&cameraWriteInterceptor), &unusedTrampoline) != MH_OK
        || MH_EnableHook(writeTarget) != MH_OK) {
        modlog::Line("CameraInterceptor: write-block hook setup failed. Reads will work but writes won't stick.");
        return true;
    }

    modlog::Line("CameraInterceptor: write-block hook installed. Press F11 to toggle camera override.");

    // A2: the blend stack's call to the record copy (FUN_140164300 at +0x164300, the function holding the write block)
    // at +0x16822E; its return address identifies the camera component's record write.
    {
        const uintptr_t copyFn = GameAddress(GameAddr::RecordCopyFn);
        const uintptr_t w = reinterpret_cast<uintptr_t>(writeTarget);
        std::string found;
        for (int k = 0; k < 4 && copyFn; ++k) {
            const uintptr_t ret = GameAddress(kAimInjectRet[k]);
            if (!ret) continue;
            const auto* call = reinterpret_cast<const unsigned char*>(ret - 5);
            if (w > copyFn && w < copyFn + 0x100 && call[0] == 0xE8 && ret + *reinterpret_cast<const int32_t*>(call + 1) == copyFn) {
                g_aimInjectRets[k] = ret;
                g_aimInjectRet = ret;
                found += std::string(" ") + kAimInjectName[k];
            }
        }
        if (!found.empty()) g_aimInjectEnabled = 1;   // other records reach ApplyAimInjection from now on (cheap when idle)
        // Aim where you look: on at startup from [aim] head_aim (default 1), in render mode aer.
        if (!found.empty() && GetHeadAimSetting() && GetRenderMode() == RenderMode::Aer) {
            g_headAim.store(true);
            modlog::Line("HEADAIM: aim where you look ON ([aim] head_aim; K toggles).");
        }
        g_prevCopyRet[0] = GameAddress(GameAddr::PrevCopyRet0);
        g_prevCopyRet[1] = GameAddress(GameAddr::PrevCopyRet1);
        modlog::Line(found.empty() ? "AIMINJECT: the blend stacks' record copies aren't where expected (different game build?) -- Ctrl+Alt+A unavailable."
                                   : "AIMINJECT: camera component record writes found:" + found + " (Ctrl+Alt+A: +20 deg yaw test).");
    }
    return true;
}

} // namespace vrmod

// Called from present_hook.cpp's hkPresent, same pattern as camera_probe.cpp.
namespace vrmod {

// Edge-triggered (fires once per press, not every frame held). Toggles
// g_cameraOverrideEnabled, which the camera write hook (camera_interceptor.asm)
// checks on every game camera write to decide whether to patch in head
// tracking + our FOV after the game's own write.
void TestCameraWriteIfHotkeyPressed() {
    static bool wasDown = false;
    bool isDown = (VRMOD_DEV_KEY(VK_F11) & 0x8000) != 0;
    bool justPressed = isDown && !wasDown;
    wasDown = isDown;

    if (!justPressed || !g_cameraStructAddress) {
        return;
    }

    g_headTrackingRestoreAfterMenu.store(false);   // a manual F11 overrides any pending automatic restore
    SetHeadTrackingEnabled(!g_cameraOverrideEnabled);
}

void SetHeadTrackingEnabled(bool enabled) {
    g_cameraOverrideEnabled = enabled ? 1 : 0;
    modlog::Line(std::string("CameraOverride (head tracking): ") + (enabled ? "ENABLED" : "disabled"));
}

bool IsHeadTrackingEnabled() {
    return g_cameraOverrideEnabled != 0;
}

void LogCameraStructIfHotkeyDown() {
    if (!(VRMOD_DEV_KEY(VK_F9) & 0x8000)) {
        return;
    }
    if (!g_cameraStructAddress) {
        return;
    }

    auto base = reinterpret_cast<uintptr_t>(g_cameraStructAddress);
    const float* pos = reinterpret_cast<const float*>(base + kPositionOffset);
    const float* quat = reinterpret_cast<const float*>(base + kQuaternionOffset);
    const float* fov = reinterpret_cast<const float*>(base + kFovOffset);

    std::ostringstream oss;
    oss << "REALCAM addr=0x" << std::hex << base << std::dec
        << std::fixed << std::setprecision(6)
        << " pos=(" << pos[0] << "," << pos[1] << "," << pos[2] << ")"
        << " quat=(" << quat[0] << "," << quat[1] << "," << quat[2] << "," << quat[3] << ")"
        << " fov=" << fov[0];
    modlog::Line(oss.str());
}

} // namespace vrmod

// Runs on the game's own thread inside its camera update, straight after the
// game wrote fresh position/rotation/FOV into the struct: composes the head
// rotation onto the game's rotation and forces our FOV. Position is left
// alone, so the camera still follows Lara. No allocation, no logging.
//
// Rotation composition: the struct holds a local->world quaternion (X right,
// Y forward, Z up, per the IGCS SOTTR reference), so a rotation expressed in
// the camera's own frame is game * delta (Hamilton product).
extern "C" void ApplyCameraHeadTracking(void* camera) {
    auto base = reinterpret_cast<uintptr_t>(camera);
    float* quat = reinterpret_cast<float*>(base + vrmod::kQuaternionOffset);
    float* fov = reinterpret_cast<float*>(base + vrmod::kFovOffset);

    // Instrumentation (Phase 1): the struct before the game's write, and the game's own values before ours.
    vrmod::CamPose prePose, gamePose;
    for (int k = 0; k < 3; ++k) prePose.p[k] = g_preWriteSnap[k];
    for (int k = 0; k < 4; ++k) prePose.q[k] = g_preWriteSnap[4 + k];
    prePose.fov = g_preWriteSnap[8];
    vrmod::ReadPose(base, &gamePose);
    for (int k = 0; k < 4; ++k) vrmod::g_gameQuat[k].store(gamePose.q[k], std::memory_order_relaxed);

    if (vrmod::g_traceArmed.load(std::memory_order_relaxed)) {
        int i = vrmod::g_traceCount.fetch_add(1);
        if (i < vrmod::kTraceMax) {
            const float* pos = reinterpret_cast<const float*>(base + vrmod::kPositionOffset);
            vrmod::TraceEntry& e = vrmod::g_trace[i];
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            e.qpc = now.QuadPart;
            e.frame = vrmod::HeadTrackingGetPresentFrame();
            for (int k = 0; k < 3; ++k) e.pos[k] = pos[k];
            for (int k = 0; k < 4; ++k) e.quat[k] = quat[k];
            e.fov = fov[0];
            if (i == vrmod::kTraceMax - 1) {
                vrmod::g_traceArmed.store(false);
                vrmod::g_traceNeedsFlush.store(true);
            }
        }
    }

    // Game frame, detected from write timing: the game usually writes the struct twice per frame ~0.1 ms apart,
    // but the first recording showed a 6-second stretch with one write per frame, so counting writes / 2 slips.
    // A gap of more than 1 ms since the previous write starts a new game frame. Measured in the Phase 4 runs
    // (42,240 writes): the two writes of one frame are always < 0.75 ms apart (almost all < 0.3 ms); when the game
    // thread catches up after a stall, consecutive frames' writes can be only 1.5-2 ms apart -- the earlier 2 ms
    // threshold merged 25 such frame pairs. Nothing fell between 0.75 and 1.5 ms. (Detected before the head pose is
    // applied: alternate-eye rendering picks the eye by it.)
    // Split frames (Phase 7 run, 24,828 writes): 82 times a frame's two writes were 1.0-3.8 ms apart (the game thread
    // delayed between them), so the gap alone counted two frames. Both writes carry the identical game pose -- as in
    // all 12,330 normal pairs, while no two different frames did. So a write after a gap of 1-5 ms that follows a
    // frame with only one write so far, with the same game pose, is that frame's second write. (A truly still camera
    // with a catch-up frame could be merged too; the pairing clamp then corrects it with one resync.)
    static uint64_t gameFrame = 0;
    static LONGLONG lastWriteQpc = 0;
    {
        static LONGLONG gapTicks = 0, splitTicks = 0;
        static int writesInFrame = 0;
        static vrmod::CamPose lastGamePose{};
        if (gapTicks == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            gapTicks = f.QuadPart / 1000;       // 1 ms
            splitTicks = f.QuadPart / 200;      // 5 ms
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const LONGLONG gap = now.QuadPart - lastWriteQpc;
        if (gap > gapTicks) {
            if (gap <= splitTicks && writesInFrame == 1 && memcmp(&gamePose, &lastGamePose, sizeof gamePose) == 0) {
                vrmod::g_splitMerges.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++gameFrame;
                writesInFrame = 0;
            }
        }
        ++writesInFrame;
        lastGamePose = gamePose;
        lastWriteQpc = now.QuadPart;
    }

    // A3: take the head-aim rotation off the slot's rotation again before anything composes the eye pose on it.
    vrmod::HeadAimUndo(quat, gameFrame);
    vrmod::CamStateProbeGameThread(vrmod::g_cameraComponent.load(std::memory_order_relaxed), quat);

    // Hold the base camera (AER stereo check, Ctrl+H): replace the game's position/rotation with the ones captured
    // when the hold started, so only head motion moves the view.
    {
        static bool held = false;
        static float heldPos[3], heldQuat[4];
        float* pos = reinterpret_cast<float*>(base + vrmod::kPositionOffset);
        if (vrmod::AerHoldBase()) {
            if (!held) {
                for (int k = 0; k < 3; ++k) heldPos[k] = pos[k];
                for (int k = 0; k < 4; ++k) heldQuat[k] = quat[k];
                held = true;
            }
            for (int k = 0; k < 3; ++k) pos[k] = heldPos[k];
            for (int k = 0; k < 4; ++k) quat[k] = heldQuat[k];
        } else {
            held = false;
        }
    }

    // First-person proof of concept: move the camera forward along the game's own view
    // direction, from the third-person boom position toward Lara's head, and up by a
    // height offset. Both values are tuned live (Home / PageUp / PageDown; see
    // PollFirstPersonHotkeys). Uses the game's rotation BEFORE the head is composed in.
    if (vrmod::g_firstPersonEnabled.load(std::memory_order_relaxed)) {
        float* pos = reinterpret_cast<float*>(base + vrmod::kPositionOffset);
        const float qx = quat[0], qy = quat[1], qz = quat[2], qw = quat[3];
        const float fx = 2.0f * (qx * qy - qw * qz);
        const float fy = 1.0f - 2.0f * (qx * qx + qz * qz);
        const float fz = 2.0f * (qy * qz + qw * qx);
        // The distance/height settings are in metres; the game's world units are either
        // metres or centimetres (auto-detected from camera speed, see PollFirstPersonHotkeys).
        const float unit = vrmod::g_worldUnitsPerMetre.load(std::memory_order_relaxed);
        const float dist = (vrmod::g_firstPersonDistance.load(std::memory_order_relaxed)
            + vrmod::g_runPushMetres.load(std::memory_order_relaxed)) * unit;
        const float height = vrmod::g_firstPersonHeight.load(std::memory_order_relaxed) * unit;
        const float side = vrmod::g_firstPersonSide.load(std::memory_order_relaxed) * unit;
        // Camera-local right axis (local +X) in world space.
        const float rx = 1.0f - 2.0f * (qy * qy + qz * qz);
        const float ry = 2.0f * (qx * qy + qw * qz);
        const float rz = 2.0f * (qx * qz - qw * qy);
        const float offsetPos[3] = { pos[0] + fx * dist + rx * side,
                                     pos[1] + fy * dist + ry * side,
                                     pos[2] + fz * dist + rz * side + height };   // world is Z-up (X right, Y forward, Z up)
        // Head-bone camera (docs/FIRSTPERSON.md): the offset position tells the locator where Lara's head roughly is;
        // once her skeleton is located the camera sits at the head bone instead.
        vrmod::HeadBonePublishHint(offsetPos);
        float eye[3];
        if (vrmod::HeadBoneEye(gameFrame, quat, eye)) {
            pos[0] = eye[0]; pos[1] = eye[1]; pos[2] = eye[2];
        } else {
            pos[0] = offsetPos[0]; pos[1] = offsetPos[1]; pos[2] = offsetPos[2];
        }
        {
            // Body turn (body_turn.cpp): her head is ahead of the point she pivots on, so turning her swept the eye sideways
            // -- 5.5-7 cm per frame (2026-10-11), and translation is what the warps and DLSS handle worst (the user saw
            // worse motion clarity while she turned). While she turns, the eye keeps its horizontal position from the frame
            // before the turn. Afterwards the leftover OFFSET from her head closes (smoothstep over 0.6 s standing, 0.15 s
            // once she moves): gliding from a fixed world point instead left the eye behind when she ran off after a turn --
            // her headless body ran into view, then the camera snapped to her neck (2026-10-11). Height stays live.
            static float lastXY[2] = {}, holdXY[2] = {}, offXY[2] = {}, liveXY[2] = {};
            static bool holding = false, releasing = false;
            static float release = 1.0f;   // 0..1 after the turn
            static LARGE_INTEGER lastQpc{};
            LARGE_INTEGER qnow, qf;
            QueryPerformanceCounter(&qnow);
            QueryPerformanceFrequency(&qf);
            float dts = lastQpc.QuadPart ? static_cast<float>(qnow.QuadPart - lastQpc.QuadPart) / static_cast<float>(qf.QuadPart) : 0.0f;
            lastQpc = qnow;
            if (dts <= 0.0f || dts > 0.1f) dts = 1.0f / 72.0f;
            // Her head's own horizontal speed (the live eye before our hold), to close the offset fast once she moves.
            const float vx = pos[0] - liveXY[0], vy = pos[1] - liveXY[1];
            const bool moving = dts > 0.002f && std::sqrt(vx * vx + vy * vy) / dts > 60.0f;   // world units (cm) per s
            liveXY[0] = pos[0]; liveXY[1] = pos[1];
            if (vrmod::BodyTurnTurning()) {
                if (!holding) { holdXY[0] = lastXY[0]; holdXY[1] = lastXY[1]; holding = true; }
                releasing = false;
                // At most 8 cm from her head (2026-10-12: held still while her head swung round the pivot, the eye ended up
                // behind her neck and saw into her body until it glided back).
                const float hx = holdXY[0] - pos[0], hy = holdXY[1] - pos[1], hd = std::sqrt(hx * hx + hy * hy);
                if (hd > 8.0f) { holdXY[0] = pos[0] + hx * 8.0f / hd; holdXY[1] = pos[1] + hy * 8.0f / hd; }
                pos[0] = holdXY[0]; pos[1] = holdXY[1];
            } else if (holding) {
                holding = false;
                releasing = true;
                release = 0.0f;
                offXY[0] = holdXY[0] - pos[0]; offXY[1] = holdXY[1] - pos[1];
            }
            if (releasing && !holding) {
                release = (std::min)(1.0f, release + dts / (moving ? 0.15f : 0.6f));
                const float t = release * release * (3.0f - 2.0f * release);
                pos[0] += offXY[0] * (1.0f - t);
                pos[1] += offXY[1] * (1.0f - t);
                if (release >= 1.0f) releasing = false;
            }
            if (holding || releasing) {
                // The hold and its glide back obey the head camera's rule too (2026-10-13): the 8 cm cap let the held eye
                // sit behind her head after a turn on the spot -- the log showed the clamp in HeadBoneEye firing in most
                // frames, then this block moving the eye back again -- and she saw her collar / back. Against the live
                // eye: at most 2 cm behind along the view's horizontal forward and 4 cm to either side; ahead is free.
                float hfx = fx, hfy = fy;
                const float hn = std::sqrt(hfx * hfx + hfy * hfy);
                if (hn > 1e-4f) {
                    hfx /= hn; hfy /= hn;
                    const float cm = unit * 0.01f;
                    const float dx = pos[0] - liveXY[0], dy = pos[1] - liveXY[1];
                    const float along = dx * hfx + dy * hfy, across = dx * hfy - dy * hfx;   // across: + = right
                    float pullF = 0.0f, pullS = 0.0f;
                    if (along < -2.0f * cm) pullF = -2.0f * cm - along;
                    if (across > 4.0f * cm) pullS = 4.0f * cm - across; else if (across < -4.0f * cm) pullS = -4.0f * cm - across;
                    pos[0] += hfx * pullF + hfy * pullS;
                    pos[1] += hfy * pullF - hfx * pullS;
                }
            }
            lastXY[0] = pos[0]; lastXY[1] = pos[1];
        }
        {
            // Neck model (2026-10-12): the camera sits in the middle of her head, so looking down pivoted the view
            // through her neck and chest from inside. Real eyes sit in front of the neck and move forward as you look
            // down: from 20 deg below horizontal the eye moves forward along the view's direction, up to
            // [firstperson] neck_forward_cm at 70 deg (smoothstep), so her body is seen from in front -- and stays drawn.
            // The view angles are head drive's from the last game frame (it runs after this).
            float vy = 0.0f, vp = 0.0f;
            const float neckCm = vrmod::GetNeckForwardCmSetting();
            if (neckCm > 0.0f && vrmod::HeadDriveViewAngles(&vy, &vp)) {
                float t = (-vp - 0.3491f) / (1.2217f - 0.3491f);   // 20 deg .. 70 deg below horizontal
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                const float push = neckCm * 0.01f * unit * t * t * (3.0f - 2.0f * t);
                pos[0] += -std::sin(vy) * push;   // world yaw as the camera's: forward = (-sin, cos)
                pos[1] += std::cos(vy) * push;
            }
        }
        // A3b: the eye relative to the game camera, in its axes (quat is the game's own rotation here).
        const float dv[3] = { pos[0] - gamePose.p[0], pos[1] - gamePose.p[1], pos[2] - gamePose.p[2] };
        const float qi[4] = { -quat[0], -quat[1], -quat[2], quat[3] };
        vrmod::QuatRotate(qi, dv, vrmod::g_eyeOffLocal);
        vrmod::g_eyeOffFrame = gameFrame;
        for (int k = 0; k < 3; ++k) vrmod::g_eyeOffLog[k].store(vrmod::g_eyeOffLocal[k], std::memory_order_relaxed);
    } else {
        vrmod::g_eyeOffFrame = 0;
    }

    // The real fix (docs/AIM.md): in first person the head turns the game camera itself; from here on `quat` is the
    // body rotation (the game's with the head part taken off), which AER composes with the full head pose as before.
    // After the first-person block, which places the head hint from the game camera as the game built it.
    vrmod::HeadDriveGameThread(vrmod::g_cameraComponent.load(std::memory_order_relaxed), gameFrame, quat,
        vrmod::GetRenderMode() == vrmod::RenderMode::Aer && vrmod::IsHeadTrackingEnabled()
            && vrmod::g_firstPersonEnabled.load(std::memory_order_relaxed) && vrmod::HeadBoneActive()
            && !vrmod::g_headAimSuspended.load(std::memory_order_relaxed));

    float d[4];
    bool haveDelta = false;
    float aerFov = 0.0f;   // AER: the FOV for this frame (Phase 6: covers the headset's view); 0 = not set yet
    if (vrmod::GetRenderMode() == vrmod::RenderMode::Aer) {
        // Alternate-eye rendering: this game frame's eye, with its own view pose (rotation and position).
        float offset[3];
        haveDelta = vrmod::AerComputeCamera(gameFrame, quat, d, offset, &aerFov);
        if (haveDelta) {
            float* pos = reinterpret_cast<float*>(base + vrmod::kPositionOffset);
            for (int k = 0; k < 3; ++k) pos[k] += offset[k];
        }
    } else {
        haveDelta = vrmod::HeadTrackingComputeGameDelta(d);
    }
    if (haveDelta) {
        const float ax = quat[0], ay = quat[1], az = quat[2], aw = quat[3];
        float x = aw * d[0] + ax * d[3] + ay * d[2] - az * d[1];
        float y = aw * d[1] - ax * d[2] + ay * d[3] + az * d[0];
        float z = aw * d[2] + ax * d[1] - ay * d[0] + az * d[3];
        float w = aw * d[3] - ax * d[0] - ay * d[1] - az * d[2];
        float n = std::sqrt(x * x + y * y + z * z + w * w);
        if (n > 0.0f) {
            float inv = 1.0f / n;
            quat[0] = x * inv;
            quat[1] = y * inv;
            quat[2] = z * inv;
            quat[3] = w * inv;
        }
    }

    // Flip test (Ctrl+Y, Phase 1/4): pitch the camera +60 / -60 degrees about its own right axis on alternate game
    // frames (by the game frame above, so both writes of a frame get the same sign). The ghost test (Ctrl+G, Phase 3)
    // can also hold either pose on every frame.
    int flipSign = 0;
    switch (vrmod::FrameLogFlipMode()) {
    case vrmod::FlipMode::Alternate: flipSign = (gameFrame % 2 == 0) ? 1 : -1; break;
    case vrmod::FlipMode::HoldPlus:  flipSign = 1; break;
    case vrmod::FlipMode::HoldMinus: flipSign = -1; break;
    case vrmod::FlipMode::Random:    flipSign = vrmod::FrameLogRandomFlipSign(gameFrame); break;
    default: break;
    }
    vrmod::g_latestGameFrame.store(gameFrame, std::memory_order_release);
    vrmod::g_cameraWriteTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    // Diagnostic (counter scan, pairing test): hold the game thread once, making this game frame late. The gap to the
    // next write is then measured from the end of the stall -- otherwise a stall between a frame's two writes splits
    // that frame into two counted frames (the first pairing run with stalls did exactly that: 49 phantom frames).
    if (const int stallMs = vrmod::g_injectStallMs.exchange(0, std::memory_order_relaxed)) {
        Sleep(static_cast<DWORD>(stallMs));
        LARGE_INTEGER after;
        QueryPerformanceCounter(&after);
        lastWriteQpc = after.QuadPart;
    }
    if (flipSign != 0) {
        const float half = 0.5f * 1.0471976f * static_cast<float>(flipSign);   // 60 degrees
        const float s = std::sin(half), c = std::cos(half);
        const float ax = quat[0], ay = quat[1], az = quat[2], aw = quat[3];
        // q * (s, 0, 0, c): rotation about the camera-local X (right) axis.
        quat[0] = aw * s + ax * c;
        quat[1] = ay * c + az * s;
        quat[2] = az * c - ay * s;
        quat[3] = aw * c - ax * s;
    }

    fov[0] = (haveDelta && aerFov > 0.0f) ? aerFov : vrmod::HeadTrackingGetGameFov();
    // Head aim off while aiming (the rope-arrow stopgap, 2026-10-01): the game aims along its own camera, which the
    // head has turned away from -- so the HUD, whose centre is the crosshair, is shifted to where that direction falls
    // in this frame's view: the game camera's forward seen through d^-1 (head yaw/pitch relative to it), projected with
    // the frame's frustum.
    {
        float ox = 0.0f, oy = 0.0f;
        float hd[4];
        if (haveDelta && aerFov > 0.0f && !vrmod::g_headAim.load(std::memory_order_relaxed) && !vrmod::HeadDriveActive() && vrmod::CameraAimingNow()
            && vrmod::AerHeadAimDelta(hd)) {
            const float qi[4] = { -hd[0], -hd[1], -hd[2], hd[3] };
            const float f[3] = { 0.0f, 1.0f, 0.0f };
            float v[3];
            vrmod::QuatRotate(qi, f, v);
            float tH = 0.0f, tV = 0.0f;
            vrmod::OpenXRPredictFrustum(aerFov, vrmod::OpenXRFovModel(), &tH, &tV);
            if (v[1] > 0.2f && tH > 0.0f && tV > 0.0f) {
                ox = v[0] / (v[1] * tH);
                oy = v[2] / (v[1] * tV);
                ox = ox < -1.0f ? -1.0f : (ox > 1.0f ? 1.0f : ox);
                oy = oy < -1.0f ? -1.0f : (oy > 1.0f ? 1.0f : oy);
            }
        }
        vrmod::UiProbeSetAimOffset(ox, oy);
    }
    if (vrmod::g_aimSync.load(std::memory_order_relaxed)) vrmod::SyncRecordMatrices(base, gamePose);
    // First person, step 5: the near-plane search writes its candidate again right after the game's write.
    vrmod::NearProbeApply();   // step 5: near-plane candidate override (Ctrl+Alt+X), if one is active

    // The game's final projection for the previous game frame (see AerRecordFinalProjection): the camera manager computes
    // it after this hook returns, so at the first write of frame F the slot holds frame F-1's. Checked for sanity.
    {
        static uint64_t lastFinalFrame = 0;
        if (gameFrame != lastFinalFrame && gameFrame > 0) {
            lastFinalFrame = gameFrame;
            const float fovV = *reinterpret_cast<const float*>(base - 0x268);
            const float aspect = *reinterpret_cast<const float*>(base - 0x264);
            if (fovV > 0.3f && fovV < 3.0f && aspect > 0.5f && aspect < 3.0f) {
                vrmod::AerRecordFinalProjection(gameFrame - 1, fovV, aspect);
            }
        }
    }

    // First-person near clip (docs/FIRSTPERSON.md). The camera manager keeps, 0x278 bytes before our struct, the
    // renderer's near and far distances (5 cm, 550000 cm) and 8 bytes on last frame's copies (FUN_1409d6f30 copies
    // [-0x26],[-0x25] -> [-0x24],[-0x23]); writing the near to 30 hid her head and shoulders. While first person is on,
    // both copies get [firstperson] near_cm; otherwise the game's own value is put back. Checked before every write:
    // near in (0.5, 100) and far > 1000, so a changed layout is left alone.
    {
        static bool overriding = false, warned = false;
        static float gameNear = 5.0f, lastWritten = 0.0f;
        float* nearNow = reinterpret_cast<float*>(base - 0x278);
        float* farNow = reinterpret_cast<float*>(base - 0x274);
        float* nearPrev = reinterpret_cast<float*>(base - 0x270);
        const bool layoutOk = *nearNow > 0.5f && *nearNow < 100.0f && *farNow > 1000.0f;
        // 2026-10-07: off by default ([firstperson] near_clip=1 brings it back) -- the head, bow and quiver are now hidden
        // by skipping their draws (ui_probe.cpp), so the game's own near plane is right again.
        static const bool nearClip = vrmod::GetNearClipSetting();
        const bool want = nearClip && vrmod::g_firstPersonEnabled.load(std::memory_order_relaxed) && vrmod::HeadBoneActive();
        if (!layoutOk) {
            if (!warned) { warned = true; vrmod::g_nearLayoutBad.store(true); }
        } else if (want) {
            if (*nearNow != lastWritten) gameNear = *nearNow;   // the game's own (it may change it per camera)
            const float n = vrmod::HeadCamNearCm();
            *nearNow = n;
            *nearPrev = n;
            lastWritten = n;
            overriding = true;
        } else if (overriding) {
            *nearNow = gameNear;
            *nearPrev = gameNear;
            lastWritten = 0.0f;
            overriding = false;
        }
    }
    // Gear hiding (workstream G): Lara's attachments hidden while the first-person head camera is in use.
    vrmod::GearHideGameThread(gameFrame, vrmod::g_firstPersonEnabled.load(std::memory_order_relaxed) && vrmod::HeadBoneActive());
    vrmod::FacingProbeGameThread();   // diagnostic: the facing probe's writes / calls, on the game thread
    vrmod::ReticleWatchGameThread(gameFrame, vrmod::g_cameraComponent.load(std::memory_order_relaxed));
    // Head hiding (ui_probe.cpp): Lara's head model isn't drawn while the first-person head camera is in use.
    {
        // Hiding (ui_probe.cpp): Lara's head model, the bow (back in view as soon as she aims) and the quiver, while the
        // first-person head camera is in use; plus the body-part test (Ctrl+Alt+F6).
        const bool fp = vrmod::g_firstPersonEnabled.load(std::memory_order_relaxed) && vrmod::HeadBoneActive()
                        && !vrmod::g_headAimSuspended.load(std::memory_order_relaxed);
        const bool gear = fp && vrmod::g_hideHeadOn.load(std::memory_order_relaxed);
        // Her body too while the view points well away from where she faces (looking back through her): head_drive.cpp.
        vrmod::UiProbeSetHideGroups(gear, gear && !vrmod::CameraAimingNow(), gear,
                                    gear && !vrmod::CameraAimingNow() && vrmod::HeadDriveLookingBack());
    }
    // Pose tag (value scan, Ctrl+T): the game frame's low 6 bits in FOV mantissa bits 2..7 (steps of 4 ulp, at most
    // 252 ulp ~ 4e-5 rad, invisible) so every copy of the FOV -- or a value computed from it, like the projection's
    // cot(fov/2), whose steps stay ~5 ulp apart -- says which frame it came from.
    if (vrmod::g_fovTagging.load(std::memory_order_relaxed)) {
        uint32_t bits;
        memcpy(&bits, fov, 4);
        bits = (bits & ~0xFFu) | (static_cast<uint32_t>(gameFrame & 0x3F) << 2);
        memcpy(fov, &bits, 4);
    }

    // D2: the final camera of this game frame (the second write of a frame overwrites the first; both are the same).
    {
        const float* p = reinterpret_cast<const float*>(base + vrmod::kPositionOffset);
        vrmod::CamFrameRec rec{ gameFrame, { p[0], p[1], p[2] }, { quat[0], quat[1], quat[2], quat[3] },
                                *reinterpret_cast<const float*>(base - 0x278), *reinterpret_cast<const float*>(base - 0x274) };
        vrmod::StoreCamFrame(rec);
    }
    // FPSWITCH (2026-10-09, "the third-person toggle: nothing happens" although first person logged off): for 4 s after
    // the first-person flag changes, where the game put the camera and where we left it, each vs Lara's drawn head.
    {
        static bool lastFp = false;
        static ULONGLONG since = 0, lastLog = 0;
        const bool fpNow = vrmod::g_firstPersonEnabled.load(std::memory_order_relaxed);
        const ULONGLONG t = GetTickCount64();
        if (fpNow != lastFp) { lastFp = fpNow; since = t; lastLog = 0; }
        if (since && t - since < 4000 && t - lastLog >= 400) {
            lastLog = t;
            const float* p = reinterpret_cast<const float*>(base + vrmod::kPositionOffset);
            float head[3];
            const bool haveHead = vrmod::FaceHeadPoint(head, 500.0);
            auto dist = [&](const float* a) {
                const float dx = a[0] - head[0], dy = a[1] - head[1], dz = a[2] - head[2];
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            };
            char b[320];
            snprintf(b, sizeof(b), "FPSWITCH +%llu ms: first person %s | game camera (%.0f,%.0f,%.0f) %s%.0f | ours (%.0f,%.0f,%.0f) %s%.0f units from the drawn head | AER %d",
                static_cast<unsigned long long>(t - since), fpNow ? "ON" : "off", gamePose.p[0], gamePose.p[1], gamePose.p[2],
                haveHead ? "" : "(no head) ", haveHead ? dist(gamePose.p) : 0.0f, p[0], p[1], p[2], haveHead ? "" : "(no head) ",
                haveHead ? dist(p) : 0.0f, haveDelta ? 1 : 0);
            modlog::Line(b);
        }
    }
    vrmod::CamPose oursPose;
    vrmod::ReadPose(base, &oursPose);
    vrmod::FrameLogCameraWrite(prePose, gamePose, oursPose, flipSign, gameFrame);

    // Phase 8: are our writes honoured? Just before the game's write the struct should still hold what we left after
    // the previous write -- in gameplay it does, bit for bit (Phase 4; Phase 7 recordings: 0 differences in 19,614
    // Presents). The first Phase 8 run found a second case: in tight spots (the opening gorge) the game ADJUSTS our
    // camera after each write -- same rotation (the quaternion scaled by 0.99988), position moved 0.3 cm median / 5.8 cm
    // max, almost identically for both eyes (left-vs-right difference 0.035 cm median) -- so the stereo pair survives.
    // Only a different camera counts as "not honoured": rotation off by more than 1 degree or position by more than
    // 10 cm. Smaller changes are counted as adjustments.
    {
        static vrmod::CamPose lastOurs{};
        static bool haveLastOurs = false;
        if (haveLastOurs && memcmp(&prePose, &lastOurs, sizeof prePose) != 0) {
            const float* a = prePose.q;
            const float* b = lastOurs.q;
            const float na = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
            const float nb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
            const float dot = (na > 0.0f && nb > 0.0f)
                ? std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) / (na * nb) : 0.0f;
            const float rotDeg = 2.0f * std::acos((std::min)(1.0f, dot)) * 57.29578f;
            const float dx = prePose.p[0] - lastOurs.p[0], dy = prePose.p[1] - lastOurs.p[1], dz = prePose.p[2] - lastOurs.p[2];
            const float posCm = std::sqrt(dx * dx + dy * dy + dz * dz);
            // Recoil / camera shake (firing switched to third person, 2026-09-26): the game turns the camera a little after
            // our write without moving it; squeezing through gaps moves it without turning (rotation identical). A
            // rotation-only change is counted separately and isn't steering.
            vrmod::StoreMax(vrmod::g_winRotCdeg, static_cast<uint32_t>(rotDeg * 100.0f));
            vrmod::StoreMax(vrmod::g_winPosMm, static_cast<uint32_t>(posCm * 10.0f));
            if (rotDeg > 0.05f && posCm < 1.0f) {
                vrmod::g_shakeWrites.fetch_add(1, std::memory_order_relaxed);
            } else if (rotDeg > 1.0f || posCm > 10.0f) {
                vrmod::g_foreignWrites.fetch_add(1, std::memory_order_relaxed);
            } else {
                vrmod::g_adjustedWrites.fetch_add(1, std::memory_order_relaxed);
                const uint32_t mm = static_cast<uint32_t>(posCm * 10.0f);
                uint32_t prev = vrmod::g_adjustMaxMm.load(std::memory_order_relaxed);
                while (mm > prev && !vrmod::g_adjustMaxMm.compare_exchange_weak(prev, mm, std::memory_order_relaxed)) {}
            }
        }
        lastOurs = oursPose;
        haveLastOurs = true;
    }

    // One-time snapshot of the camera struct around the FOV (where a near/far plane would live); logged from the
    // poll thread, never from inside this hook.
    static int calls = 0;
    if (++calls == 900 && !vrmod::g_structSnapshotReady.load()) {
        const float* p = reinterpret_cast<const float*>(base);
        for (int i = 0; i < 64; ++i) vrmod::g_structSnapshot[i] = p[(0x80 / 4) + i];
        vrmod::g_structSnapshotReady.store(true);
    }
}

namespace vrmod {

namespace {
constexpr int kReasonCount = static_cast<int>(ThirdPersonReason::Count);
const char* const kReasonNames[kReasonCount] = { "cutscene", "wall crawl", "grapple", "rope/zip", "ledge/climb", "interaction", "rappel", "finisher", "diagnostic test", "scripted camera" };
std::atomic<double> g_reasonUntilMs[kReasonCount] = {};   // 0 = inactive
std::atomic<bool> g_fpRestoreAfterReasons{false};
std::atomic<bool> g_autoThirdPerson{false};

double CamNowMs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

bool AnyReasonActive(std::string* which = nullptr) {
    const double now = CamNowMs();
    for (int i = 0; i < kReasonCount; ++i) {
        const double until = g_reasonUntilMs[i].load();
        if (until > now) {
            if (which) *which = kReasonNames[i];
            return true;
        }
    }
    return false;
}
} // namespace

void CameraReasonBegin(ThirdPersonReason reason, double timeoutSeconds) {
    g_reasonUntilMs[static_cast<int>(reason)].store(CamNowMs() + timeoutSeconds * 1000.0);
}

void CameraReasonPulse(ThirdPersonReason reason, double holdSeconds) {
    auto& slot = g_reasonUntilMs[static_cast<int>(reason)];
    const double until = CamNowMs() + holdSeconds * 1000.0;
    double cur = slot.load();
    while (until > cur && !slot.compare_exchange_weak(cur, until)) {}
}

void CameraReasonEnd(ThirdPersonReason reason) {
    g_reasonUntilMs[static_cast<int>(reason)].store(0.0);
}

// ---- menus / map / loading: head tracking off, restored afterwards ----
// The game's own "is it safe to open Ansel" check (FUN_1401d1670, which logs "Active activated: playing: %d
// menus-open %d movies-playing %d") reads exactly the state we need:
//   playing   : vfunc +0x80 of the object returned by FUN_14064bf60() equals 5 while in gameplay
//   menus-open: the int at (*(SOTTR.exe+0x146BF38) + 0x7A8) is >= 0 while any menu is open
//   movie     : FUN_140224e00(*(SOTTR.exe+0x1F3AAE0)) is non-zero while a video plays
// (addresses of the known build; found by pattern -- game_addresses.cpp)
// While not in plain gameplay the head-tracked camera is switched off (the picture then follows your head, so
// menus and the map can be looked at) and restored when gameplay resumes. Every read is guarded by SEH.
namespace {

bool SafeReadU64(uintptr_t addr, uint64_t* out) {
    __try { *out = *reinterpret_cast<volatile uint64_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeReadI32(uintptr_t addr, int32_t* out) {
    __try { *out = *reinterpret_cast<volatile int32_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeCallMovies(uintptr_t fn, uint64_t arg, int* out) {
    __try { *out = reinterpret_cast<char(__fastcall*)(uint64_t)>(fn)(arg) != 0; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeCallState(uintptr_t getObj, int* out) {
    __try {
        void* obj = reinterpret_cast<void*(__fastcall*)()>(getObj)();
        if (!obj) return false;
        const uintptr_t vtbl = *reinterpret_cast<uintptr_t*>(obj);
        const uintptr_t fn = *reinterpret_cast<uintptr_t*>(vtbl + 0x80);
        *out = reinterpret_cast<int(__fastcall*)(void*)>(fn)(obj);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct GameUi {
    bool haveState = false, havemenu = false, haveMovie = false;
    int state = 0;
    bool menuOpen = false;
    bool movie = false;
    bool gameplay() const {
        if (haveState && state != 5) return false;
        if (havemenu && menuOpen) return false;
        if (haveMovie && movie) return false;
        return true;
    }
};

GameUi ReadGameUi() {
    GameUi ui;
    static const uintptr_t menus = GameAddress(GameAddr::MenusGlobal), moviesArgAt = GameAddress(GameAddr::MoviesGlobal),
                           moviesFn = GameAddress(GameAddr::MoviesFn), stateFn = GameAddress(GameAddr::GameStateFn);
    uint64_t objPtr = 0;
    int32_t v = 0;
    if (menus && SafeReadU64(menus, &objPtr) && objPtr && SafeReadI32(objPtr + 0x7A8, &v)) {
        ui.havemenu = true;
        ui.menuOpen = v >= 0;
    }
    uint64_t moviesArg = 0;
    int m = 0;
    if (moviesArgAt && moviesFn && SafeReadU64(moviesArgAt, &moviesArg) && SafeCallMovies(moviesFn, moviesArg, &m)) {
        ui.haveMovie = true;
        ui.movie = m != 0;
    }
    int st = 0;
    if (stateFn && SafeCallState(stateFn, &st)) {
        ui.haveState = true;
        ui.state = st;
    }
    return ui;
}

// Latest reading, for the frame recorder (-1 = unknown).
std::atomic<int> g_uiState{-1}, g_uiMenu{-1}, g_uiMovie{-1};

void PollGameUiState() {
    static int badFrames = 0, goodFrames = 0;
    static bool inMenu = false;
    static GameUi last;
    static bool first = true;

    const GameUi ui = ReadGameUi();
    g_uiState.store(ui.haveState ? ui.state : -1);
    g_uiMenu.store(ui.havemenu ? (ui.menuOpen ? 1 : 0) : -1);
    g_uiMovie.store(ui.haveMovie ? (ui.movie ? 1 : 0) : -1);
    if (first || ui.haveState != last.haveState || ui.state != last.state || ui.menuOpen != last.menuOpen || ui.movie != last.movie) {
        first = false;
        last = ui;
        std::ostringstream oss;
        oss << "GameState: playing-state=" << (ui.haveState ? std::to_string(ui.state) : "?")
            << " menus-open=" << (ui.havemenu ? (ui.menuOpen ? "1" : "0") : "?")
            << " movie=" << (ui.haveMovie ? (ui.movie ? "1" : "0") : "?");
        modlog::Line(oss.str());
    }

    // Debounce: entering a menu takes 3 consecutive frames, leaving takes 10 (avoids flicker at transitions).
    if (!ui.gameplay()) { ++badFrames; goodFrames = 0; } else { ++goodFrames; badFrames = 0; }
    // A level load: the break shows a playing-state other than 5 (3 = loading, 2 = main menu); a pause menu keeps 5.
    static bool sawLoad = false;
    if (inMenu && ui.haveState && ui.state != 5) sawLoad = true;
    if (!inMenu && badFrames >= 3) {
        inMenu = true;
        g_menuScreenActive.store(true);
        const bool wasOn = IsHeadTrackingEnabled();
        g_headTrackingRestoreAfterMenu.store(wasOn);
        if (wasOn) {
            SetHeadTrackingEnabled(false);
            HeadTrackingMarkDiscontinuity("entered a menu/loading");
            ViewCue(Cue::Off);
        }
        modlog::Line(std::string("Menu: head tracking off for the menu/map/loading") + (wasOn ? " (will be restored)" : " (was already off)"));
    } else if (inMenu && goodFrames >= 10) {
        inMenu = false;
        g_menuScreenActive.store(false);
        // An examine/interaction ends with its menu closing; none of the interaction's end events fire for it, so
        // run 8 stayed in third person until the 25 s timeout. End the interaction's third person here.
        CameraReasonEnd(ThirdPersonReason::Interact);
        if (sawLoad) {
            // After a level load: recentre (as F8) and re-find Lara's skeleton for the head-bone camera.
            sawLoad = false;
            HeadTrackingRequestRecenter();
            if (GetRenderMode() == RenderMode::Aer) AerRequestRecenter("level loaded");
            HeadBoneForget("level loaded");
            modlog::Line("Menu: a level was loaded -- recentred.");
        }
        if (g_headTrackingRestoreAfterMenu.exchange(false)) {
            SetHeadTrackingEnabled(true);
            HeadTrackingMarkDiscontinuity("left the menu");
            ViewCue(Cue::On);
            modlog::Line("Menu: back in gameplay, head tracking restored.");
        } else {
            modlog::Line("Menu: back in gameplay.");
        }
    }
}

} // namespace

// Home toggles first-person. PageUp/PageDown change the forward distance by 5 cm per
// tick (held keys repeat); hold Shift to change the height instead. Every change is
// logged as "FirstPerson: ..." so a good setting can be read back from the log.
uint64_t CameraOtherStructWrites() {
    return g_cameraOtherWrites;
}

bool CameraFrameRecord(uint64_t frame, CamFrameRec* out) {
    std::lock_guard<std::mutex> lock(g_camFrameMutex);
    const CamFrameRec& r = g_camFrames[frame % 16];
    if (r.frame != frame || frame == 0) return false;
    *out = r;
    return true;
}

uint64_t CameraLatestGameFrame() {
    return g_latestGameFrame.load(std::memory_order_acquire);
}

uint64_t CameraSplitMerges() {
    return g_splitMerges.load(std::memory_order_relaxed);
}

bool IsFirstPersonEnabled() {
    return g_firstPersonEnabled.load(std::memory_order_relaxed);
}

namespace {
std::atomic<bool> g_fpToggleRequest{ false };   // the overlay's First person switch (handled with the gamepad combo)
}

void RequestFirstPersonToggle() {
    g_fpToggleRequest.store(true);
}

bool HeadAimEnabled() {
    return g_headAim.load(std::memory_order_relaxed);
}

void SetHeadAimEnabled(bool on, const char* how) {
    if (g_headAim.load() != on) ToggleHeadAim(how);
}

bool HideHeadEnabled() {
    return g_hideHeadOn.load(std::memory_order_relaxed);
}

void SetHideHeadEnabled(bool on, const char* how) {
    if (g_hideHeadOn.exchange(on) == on) return;
    modlog::Line(std::string("HEADHIDE: Lara's head, bow and quiver ") + (on ? "hidden in first person" : "shown") + " (" + how + ")");
}

uint64_t CameraForeignWrites() {
    return g_foreignWrites.load(std::memory_order_relaxed);
}

void CameraGameQuat(float out[4]) {
    for (int k = 0; k < 4; ++k) out[k] = g_gameQuat[k].load(std::memory_order_relaxed);
}

uintptr_t CameraOwnerAddress() {
    return reinterpret_cast<uintptr_t>(g_cameraOwnerAddress);
}

uint64_t CameraAdjustedWrites() {
    return g_adjustedWrites.load(std::memory_order_relaxed);
}

float CameraTakeAdjustMaxCm() {
    return static_cast<float>(g_adjustMaxMm.exchange(0, std::memory_order_relaxed)) / 10.0f;
}

uint32_t CameraWriteThreadId() {
    return g_cameraWriteTid.load(std::memory_order_relaxed);
}

void CameraSetFovTagging(bool on) {
    g_fovTagging.store(on, std::memory_order_relaxed);
}

void CameraInjectStall(int ms) {
    g_injectStallMs.store(ms, std::memory_order_relaxed);
}

uintptr_t CameraStructAddress() {
    return reinterpret_cast<uintptr_t>(g_cameraStructAddress);
}

void GameUiSnapshot(int* state, int* menuOpen, int* movie) {
    *state = g_uiState.load();
    *menuOpen = g_uiMenu.load();
    *movie = g_uiMovie.load();
}

bool CameraReadStructPose(CamPose* out) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_cameraStructAddress);
    return base != 0 && ReadPoseGuarded(base, out);
}

bool MenuScreenActive() {
    return g_menuScreenActive.load();
}

// Gamepad toggle for first person (2026-09-27, the user plays with a gamepad): LB held + right stick click. XInput is
// loaded on first use; only the connected pad is read each frame (XInputGetState on an empty slot is slow), empty slots
// are re-checked every 2 s.
// Gamepad combos while LB is held: right stick click = first person (bit 0), left stick click = stabilisation (bit 1).
// Returns the combos newly pressed.
int PadCombos() {
    using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static GetState_t getState = [] {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        return m ? reinterpret_cast<GetState_t>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    if (!getState) return 0;
    // Every connected controller's buttons, combined: the first connected slot isn't necessarily the user's pad (a
    // virtual pad such as ViGEm's can hold slot 0 -- 2026-10-04 the combo stopped working). Disconnected slots are
    // slow to query, so the set is rescanned every 2 s.
    static unsigned connected = 0;
    static ULONGLONG lastScan = 0;
    static bool comboWas = false;
    const ULONGLONG scanNow = GetTickCount64();
    if (scanNow - lastScan >= 2000) {
        lastScan = scanNow;
        unsigned found = 0;
        XINPUT_STATE probe{};
        for (DWORD i = 0; i < 4; ++i) if (getState(i, &probe) == ERROR_SUCCESS) found |= 1u << i;
        if (found != connected) {
            modlog::Line("Gamepad: XInput slots connected now: " + std::to_string(found & 1) + std::to_string((found >> 1) & 1)
                + std::to_string((found >> 2) & 1) + std::to_string((found >> 3) & 1) + " (slots 0-3; all are read)");
            connected = found;
        }
    }
    if (!connected) return 0;
    WORD b = 0;
    for (DWORD i = 0; i < 4; ++i) {
        if (!(connected & (1u << i))) continue;
        XINPUT_STATE st{};
        if (getState(i, &st) == ERROR_SUCCESS) b |= st.Gamepad.wButtons;
        else connected &= ~(1u << i);
    }
    const bool combo = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) && (b & XINPUT_GAMEPAD_RIGHT_THUMB);
    const bool combo2 = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) && (b & XINPUT_GAMEPAD_RIGHT_THUMB) == 0 && (b & XINPUT_GAMEPAD_LEFT_THUMB);
    static bool combo2Was = false;
    // LB + right stick click: a short click (released within 0.6 s) is 1 (first person, on release); held 0.6 s it is 4
    // (head aim, 2026-10-01 user: rope arrows need head aim off for now) and the release does nothing.
    static ULONGLONG comboSince = 0;
    static bool holdFired = false;
    int pressed = combo2 && !combo2Was ? 2 : 0;
    const ULONGLONG now = GetTickCount64();
    if (combo && !comboWas) { comboSince = now; holdFired = false; }
    if (combo && !holdFired && now - comboSince >= 600) { holdFired = true; pressed |= 4; }
    if (!combo && comboWas && !holdFired) pressed |= 1;
    comboWas = combo;
    combo2Was = combo2;
    return pressed;
}

void PollFirstPersonHotkeys() {
    PollAimSync();
    CamModeWatch();
    EventMarkPollHotkey();
    g_headAimSuspended.store(MenuScreenActive() || CinematicActive() || AnyReasonActive(), std::memory_order_relaxed);
    static bool homeWasDown = false;
    static bool nearWarned = false;
    if (g_nearLayoutBad.load() && !nearWarned) {
        nearWarned = true;
        modlog::Line("HEADCAM: the near/far values before the camera struct don't look right (near not 0.5-100, far not > 1000); "
                     "first-person near clip NOT applied (different game build?).");
    }
    static bool dirty = false;
    static bool loaded = false;
    static Settings settings;
    PollGameUiState();
    StereoScalePollHotkeys();
    if (g_structSnapshotReady.load() && !g_structSnapshotLogged.exchange(true)) {
        std::ostringstream o;
        o << "CAMSTRUCT floats from +0x80:";
        for (int i = 0; i < 64; ++i) {
            if (i % 4 == 0) o << "\n  +0x" << std::hex << (0x80 + i * 4) << std::dec << ":";
            o << " " << g_structSnapshot[i];
        }
        modlog::Line(o.str());
    }
    static bool digitWasDown[10] = {};
    auto down = [](int vk) {   // release build: only the modifiers and Home are read (build_config.h)
        const bool real = vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_HOME;
        return ((real ? GetAsyncKeyState(vk) : VRMOD_DEV_KEY(vk)) & 0x8000) != 0;
    };

    if (!loaded) {
        loaded = true;
        settings = LoadSettings();
        g_firstPersonEnabled.store(settings.firstPerson);
        g_firstPersonDistance.store(settings.distanceMetres);
        g_firstPersonHeight.store(settings.heightMetres);
        g_firstPersonSide.store(settings.sideMetres);
        HeadTrackingSetLatencyDefault(settings.latencyFrames);
        HeadTrackingSetOrdinalDefault(settings.ordinalMatching);
        HeadTrackingSetFovDefault(settings.gameFov);
        std::ostringstream oss;
        oss << "Settings: startup first_person=" << settings.firstPerson << " distance=" << settings.distanceMetres
            << " m height=" << settings.heightMetres << " m latency=" << settings.latencyFrames;
        modlog::Line(oss.str());
    }

    // Camera profiles: Alt+1..4 recall, Ctrl+Alt+1..4 save the current distance/height,
    // Ctrl+Alt+0 saves the current values (plus first-person state and latency) as the startup default.
    {
        const bool alt = down(VK_MENU);
        const bool ctrl = down(VK_CONTROL);
        for (int digit = 0; digit <= 4; ++digit) {
            const bool isDown = alt && down('0' + digit);
            const bool pressed = isDown && !digitWasDown[digit];
            digitWasDown[digit] = isDown;
            if (!pressed) continue;
            if (ctrl) {
                if (digit == 0) {
                    settings.firstPerson = g_firstPersonEnabled.load();
                    settings.distanceMetres = g_firstPersonDistance.load();
                    settings.heightMetres = g_firstPersonHeight.load();
                    settings.sideMetres = g_firstPersonSide.load();
                    settings.latencyFrames = HeadTrackingGetLatency();
                    settings.ordinalMatching = HeadTrackingOrdinalMatchingWanted();
                    settings.gameFov = HeadTrackingGetGameFov();
                    SaveSettings(settings);
                    modlog::Line("Settings: current camera + latency + zoom (game_fov=" + std::to_string(settings.gameFov)
                        + ") saved as the startup default.");
                } else {
                    CameraProfile& p = settings.profiles[digit - 1];
                    p.valid = true;
                    p.distanceMetres = g_firstPersonDistance.load();
                    p.heightMetres = g_firstPersonHeight.load();
                    p.sideMetres = g_firstPersonSide.load();
                    SaveSettings(settings);
                    modlog::Line("Settings: saved camera profile " + std::to_string(digit));
                }
            } else if (digit >= 1) {
                const CameraProfile& p = settings.profiles[digit - 1];
                if (p.valid) {
                    g_firstPersonDistance.store(p.distanceMetres);
                    g_firstPersonHeight.store(p.heightMetres);
                    g_firstPersonSide.store(p.sideMetres);
                    g_firstPersonEnabled.store(true);
                    modlog::Line("FirstPerson: recalled profile " + std::to_string(digit) + " distance=" + std::to_string(p.distanceMetres)
                        + " m height=" + std::to_string(p.heightMetres) + " m");
                } else {
                    modlog::Line("FirstPerson: profile " + std::to_string(digit) + " is empty (save with Ctrl+Alt+" + std::to_string(digit) + ").");
                }
            }
        }
    }

    // Once a second: log the camera position and its speed, and work out the world unit from
    // it (a running character is a few m/s, so > ~50 units/s means centimetres).
    if (g_cameraStructAddress) {
        static double lastMs = 0.0;
        static float lastPos[3] = {};
        static int fastSamples = 0;
        static bool unitDecided = false;
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        const double nowMs = static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        if (nowMs - lastMs >= 1000.0) {
            const float* p = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(g_cameraStructAddress) + kPositionOffset);
            const float dx = p[0] - lastPos[0], dy = p[1] - lastPos[1], dz = p[2] - lastPos[2];
            const float speed = lastMs > 0.0 ? std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f / static_cast<float>(nowMs - lastMs) : 0.0f;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << "CAMPOS pos=(" << p[0] << "," << p[1] << "," << p[2]
                << ") speed=" << speed << " units/s";
            if (GetLogSwitches().campos) {
                modlog::Line(oss.str());
            }
            if (!unitDecided && speed > 50.0f && speed < 5000.0f && ++fastSamples >= 3) {
                unitDecided = true;
                g_worldUnitsPerMetre.store(100.0f);
                modlog::Line("FirstPerson: camera moves at hundreds of units/s, so world units look like CENTIMETRES (x100). Ctrl+Home flips this if wrong.");
            }
            lastPos[0] = p[0]; lastPos[1] = p[1]; lastPos[2] = p[2];
            lastMs = nowMs;
        }
    }

    // Speed estimate for the run-push (see g_runPushMetres above): every call, not once a second. Smoothed with a
    // ~600 ms time constant -- longer than one running stride (~300-500 ms) -- so the push tracks "am I sustaining
    // a run" rather than each stride's own speed-up/slow-down. A too-short time constant here (150 ms, the first
    // version) let the per-stride oscillation straight through, so the push pulsed in and out with each footfall
    // instead of holding steady -- the "staccato" the user reported.
    if (g_cameraStructAddress) {
        static double lastFastMs = 0.0;
        static float lastFastPos[3] = {};
        static float smoothedSpeed = 0.0f;   // metres/s
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        const double nowMs = static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        const double dtMs = nowMs - lastFastMs;
        // The game writes the struct twice per frame with identical values (~0.1 ms apart); skip those so the
        // computed dt is a real frame interval, not near-zero.
        if (lastFastMs > 0.0 && dtMs > 4.0 && dtMs < 500.0) {
            const float* p = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(g_cameraStructAddress) + kPositionOffset);
            const float dx = p[0] - lastFastPos[0], dy = p[1] - lastFastPos[1], dz = p[2] - lastFastPos[2];
            const float unit = g_worldUnitsPerMetre.load(std::memory_order_relaxed);
            const float instSpeed = (unit > 0.0f)
                ? std::sqrt(dx * dx + dy * dy + dz * dz) / unit * 1000.0f / static_cast<float>(dtMs) : 0.0f;
            const float alpha = static_cast<float>(1.0 - std::exp(-dtMs / 600.0));
            smoothedSpeed += (instSpeed - smoothedSpeed) * alpha;
            const float t01 = (smoothedSpeed - kRunThresholdMps) / kRunRampMps;
            const float clamped = t01 < 0.0f ? 0.0f : (t01 > 1.0f ? 1.0f : t01);
            const float push = clamped * g_runPushMaxMetres.load(std::memory_order_relaxed);
            g_runPushMetres.store(push);
            if (GetLogSwitches().campos) {
                static double lastLogMs = 0.0;
                if (nowMs - lastLogMs >= 500.0) {
                    lastLogMs = nowMs;
                    modlog::Line("RunPush: speed=" + std::to_string(smoothedSpeed) + " m/s push=" + std::to_string(push) + " m");
                }
            }
            lastFastPos[0] = p[0]; lastFastPos[1] = p[1]; lastFastPos[2] = p[2];
        }
        lastFastMs = nowMs;
    }

    // Game-steered camera -> third person. The Phase 8 read-back counts camera writes the game adjusted or replaced
    // after us. In the gorge runs this happened on every write through the scripted stretches (also with first person
    // off, so switching doesn't make it stop), and the user reports it when Lara squeezes through gaps. Sustained
    // steering (>= 8 such writes in 250 ms; steady steering gives ~36) holds the reason for 2 s after the last burst,
    // so a stray single adjustment doesn't flip the view.
    {
        static double lastCheckMs = 0.0;
        static uint64_t lastSteered = 0;
        const double nowMs = CamNowMs();
        if (nowMs - lastCheckMs >= 250.0) {
            static uint64_t lastShake = 0;
            static double lastBurstLogMs = 0.0;
            const uint64_t steered = CameraAdjustedWrites() + CameraForeignWrites();
            const uint64_t shake = g_shakeWrites.load(std::memory_order_relaxed);
            const float rotDeg = g_winRotCdeg.exchange(0) / 100.0f, posCm = g_winPosMm.exchange(0) / 10.0f;
            const bool steering = lastCheckMs > 0.0 && steered - lastSteered >= 8;
            const bool shaking = lastCheckMs > 0.0 && shake - lastShake >= 8;
            // Sustained steering only (>= 6 windows in a row = 1.5 s): a shot adjusts the camera on every write for about a
            // second (2026-09-26 runs: 23-38 writes per 250 ms, <= 0.03 deg, <= 1.4 cm) -- the same signature as squeezing
            // through a gap, which lasts seconds.
            static int steeringRun = 0;
            steeringRun = steering ? steeringRun + 1 : 0;
            // Log only (2026-09-26): the camera-mode watch decides now -- these tiny adjustments (<= 0.05 deg, <= 1.5 cm)
            // also happen in ordinary play and while aiming, and switched first person off at random.
            if ((steering || shaking) && nowMs - lastBurstLogMs >= 1000.0) {
                lastBurstLogMs = nowMs;
                std::ostringstream o;
                o.setf(std::ios::fixed);
                o.precision(2);
                o << "GAMECAM (250 ms): " << steered - lastSteered << " steered writes, " << shake - lastShake
                  << " rotation-only (recoil/shake, ignored); largest change " << rotDeg << " deg, " << posCm << " cm"
                  << (steeringRun >= 6 ? " (sustained; log only)" : steering ? " (not sustained)" : "");
                modlog::Line(o.str());
            }
            lastSteered = steered;
            lastShake = shake;
            lastCheckMs = nowMs;
        }
    }

    // Event-driven third person: switch first-person off when a third-person moment begins (cutscene, climb,
    // grapple, ...), and restore it when the last one clears. Only the transitions act, so pressing Home during
    // such a moment is respected (it cancels the automatic restore instead of fighting it).
    {
        std::string which;
        const bool any = AnyReasonActive(&which);
        static bool quiet = false;   // the ghost test's switch is silent: its own tones report the test
        if (any && !g_autoThirdPerson.load()) {
            g_autoThirdPerson.store(true);
            quiet = which == kReasonNames[static_cast<int>(ThirdPersonReason::Diagnostic)];
            const bool wasOn = g_firstPersonEnabled.load();
            g_fpRestoreAfterReasons.store(wasOn);
            if (wasOn) {
                g_firstPersonEnabled.store(false);
                if (!quiet) ViewCue(Cue::Off);
            }
            modlog::Line("ThirdPerson: switching for '" + which + "'" + (wasOn ? " (first-person was on, will be restored)" : " (first-person was already off)"));
        } else if (!any && g_autoThirdPerson.load()) {
            g_autoThirdPerson.store(false);
            if (g_fpRestoreAfterReasons.exchange(false)) {
                g_firstPersonEnabled.store(true);
                if (!quiet) ViewCue(Cue::On);
                modlog::Line("ThirdPerson: moment over, first-person restored.");
            } else {
                modlog::Line("ThirdPerson: moment over.");
            }
        }
    }

    // Release build: plain Home only (first / third person); its Ctrl / Alt variants are development keys.
    const bool homeDown = down(VK_HOME) && (VRMOD_DIAG || (!down(VK_CONTROL) && !down(VK_MENU)));
    if (homeDown && !homeWasDown) {
        if (down(VK_CONTROL) && down(VK_MENU)) {
            HeadBoneToggle();   // Ctrl+Alt+Home: head-bone camera on/off (off = the distance/height/side offsets)
        } else if (down(VK_CONTROL)) {
            // Ctrl+Home: reset the camera position to the saved startup default.
            g_firstPersonDistance.store(settings.distanceMetres);
            g_firstPersonHeight.store(settings.heightMetres);
            g_firstPersonSide.store(settings.sideMetres);
            modlog::Line("FirstPerson: RESET to the startup default distance=" + std::to_string(settings.distanceMetres)
                + " m height=" + std::to_string(settings.heightMetres) + " m side=" + std::to_string(settings.sideMetres) + " m");
        } else if (down(VK_MENU)) {
            // Alt+Home: cycle the run-push stopgap's maximum (see g_runPushMaxMetres above).
            static const float kSteps[] = { 0.0f, 0.3f, 0.5f, 0.6f, 0.7f, 0.9f };
            constexpr int kNumSteps = sizeof(kSteps) / sizeof(kSteps[0]);
            const float cur = g_runPushMaxMetres.load();
            int idx = 0;
            for (int i = 0; i < kNumSteps; ++i) if (std::fabs(kSteps[i] - cur) < 0.001f) idx = i;
            const float next = kSteps[(idx + 1) % kNumSteps];
            g_runPushMaxMetres.store(next);
            modlog::Line("FirstPerson: run-push max now " + std::to_string(next) + " m (extra forward push while running, to outrun the head bob).");
            PlayCue(Cue::On);
        } else {
            g_firstPersonEnabled.store(!g_firstPersonEnabled.load());
            g_fpRestoreAfterReasons.store(false);   // a manual toggle overrides any pending automatic restore
            ViewCue(g_firstPersonEnabled.load() ? Cue::On : Cue::Off);
            dirty = true;
        }
    }
    homeWasDown = homeDown;
    {
        // Head hiding: Ctrl+Alt+F5 toggles; a 10 s log line while first person is on.
        static bool loaded = false, f5Was = false;
        if (!loaded) { loaded = true; g_hideHeadOn.store(GetHideHeadSetting()); }
        const bool f5 = down(VK_F5) && down(VK_CONTROL) && down(VK_MENU);
        if (f5 && !f5Was) {
            const bool on = !g_hideHeadOn.load();
            g_hideHeadOn.store(on);
            modlog::Line(std::string("HEADHIDE: Lara's head, bow and quiver ") + (on ? "hidden in first person" : "shown") + " (Ctrl+Alt+F5)");
            PlayCue(on ? Cue::On : Cue::Off);
        }
        f5Was = f5;
        static bool f6Was = false;
        const bool f6 = down(VK_F6) && down(VK_CONTROL) && down(VK_MENU);
        if (f6 && !f6Was) {
            modlog::Line("HEADHIDE: " + UiProbeGearTestStep() + " (Ctrl+Alt+F6)");
            PlayCue(UiProbeGearTestIndex() ? Cue::On : Cue::Off);
        }
        f6Was = f6;
        static ULONGLONG last = 0;
        const ULONGLONG nowT = GetTickCount64();
        if (nowT - last >= 10000) {
            last = nowT;
            const std::string stats = UiProbeHideStats();
            if (g_firstPersonEnabled.load()) {
                modlog::Line(std::string("HEADHIDE (10 s, ") + (g_hideHeadOn.load() ? "on" : "off") + ", gear finder "
                    + std::to_string(UiProbeGearTestIndex()) + "): " + stats);
            }
        }
    }
    const int padCombos = PadCombos() | (g_fpToggleRequest.exchange(false) ? 1 : 0);
    if (padCombos & 2) HeadBoneToggleStabilise();   // gamepad: hold LB, click the left stick
    if (padCombos & 4) ToggleHeadAim("gamepad LB + right stick click held");
    if (padCombos & 1) {   // gamepad: hold LB, click the right stick -- the same toggle as Home
        g_firstPersonEnabled.store(!g_firstPersonEnabled.load());
        g_fpRestoreAfterReasons.store(false);
        ViewCue(g_firstPersonEnabled.load() ? Cue::On : Cue::Off);
        modlog::Line(std::string("FirstPerson: ") + (g_firstPersonEnabled.load() ? "ON" : "off") + " (gamepad LB + right stick click)");
        dirty = true;
    }

    // No Shift combinations (on the user's keyboard Shift+PageUp/PageDown triggers Bluetooth
    // pairing). Distance: PageUp/PageDown.  Height: Alt+PageUp/PageDown or Ctrl+Up/Down arrows.
    // Sideways: Ctrl+PageUp/PageDown or Ctrl+Right/Left arrows.
    const bool ctrlOnly = down(VK_CONTROL) && !down(VK_MENU);
    const bool altOnly = down(VK_MENU) && !down(VK_CONTROL);
    float pageStep = 0.0f;
    if (down(VK_PRIOR)) pageStep = 0.05f;    // PageUp
    if (down(VK_NEXT)) pageStep = -0.05f;    // PageDown
    float upDownStep = 0.0f, leftRightStep = 0.0f;
    if (ctrlOnly) {
        if (down(VK_UP)) upDownStep = 0.05f;
        if (down(VK_DOWN)) upDownStep = -0.05f;
        if (down(VK_RIGHT)) leftRightStep = 0.05f;
        if (down(VK_LEFT)) leftRightStep = -0.05f;
    }
    // Sensible limits so a stuck or repeated key can't run the camera away (it once
    // reached -134 m); Ctrl+Home resets to the saved startup default.
    auto clampTo = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    // Head-bone camera active: the same keys move the eye offset from the head bone, 1 cm per press (repeating every
    // 150 ms while held) -- the offsets below only matter for the fallback.
    if (HeadBoneActive() && g_firstPersonEnabled.load()) {
        // Near clip: Ctrl+Alt+] / Ctrl+Alt+[ (Ctrl+Alt+PageUp never arrived on the user's system; PageDown did).
        static bool rbWas = false, lbWas = false;
        const bool ca = down(VK_CONTROL) && down(VK_MENU);
        const bool rb = down(VK_OEM_6), lb = down(VK_OEM_4);
        if (ca && rb && !rbWas) HeadCamAdjustNear(1.0f);
        if (ca && lb && !lbWas) HeadCamAdjustNear(-1.0f);
        rbWas = rb;
        lbWas = lb;
        static double nextRepeatMs = 0.0;
        static bool wasHeld = false;
        const bool held = pageStep != 0.0f || upDownStep != 0.0f || leftRightStep != 0.0f;
        const double nowMs = static_cast<double>(GetTickCount64());
        if (held && (!wasHeld || nowMs >= nextRepeatMs)) {
            nextRepeatMs = nowMs + (wasHeld ? 150.0 : 400.0);
            const float s = 1.0f;
            float dF = 0.0f, dU = 0.0f, dS = 0.0f;
            if (pageStep != 0.0f && down(VK_CONTROL) && down(VK_MENU)) {
                HeadCamAdjustNear(pageStep > 0.0f ? 1.0f : -1.0f);   // Ctrl+Alt+PageUp/PageDown: near clip
                wasHeld = held;
                return;
            }
            if (pageStep != 0.0f) {
                const float sg = pageStep > 0.0f ? s : -s;
                if (ctrlOnly) dS = sg; else if (altOnly) dU = sg; else dF = sg;
            }
            if (upDownStep != 0.0f) dU = upDownStep > 0.0f ? s : -s;
            if (leftRightStep != 0.0f) dS = leftRightStep > 0.0f ? s : -s;
            HeadBoneAdjustEye(dF, dU, dS);
        }
        wasHeld = held;
        return;
    }
    bool changed = false;
    if (pageStep != 0.0f) {
        if (ctrlOnly) {
            g_firstPersonSide.store(clampTo(g_firstPersonSide.load() + pageStep, -1.5f, 1.5f));
        } else if (altOnly) {
            g_firstPersonHeight.store(clampTo(g_firstPersonHeight.load() + pageStep, -1.5f, 1.5f));
        } else {
            g_firstPersonDistance.store(clampTo(g_firstPersonDistance.load() + pageStep, -0.5f, 6.0f));
        }
        changed = true;
    }
    if (upDownStep != 0.0f) {
        g_firstPersonHeight.store(clampTo(g_firstPersonHeight.load() + upDownStep, -1.5f, 1.5f));
        changed = true;
    }
    if (leftRightStep != 0.0f) {
        g_firstPersonSide.store(clampTo(g_firstPersonSide.load() + leftRightStep, -1.5f, 1.5f));
        changed = true;
    }
    if (changed) {
        dirty = true;
    } else if (dirty && !homeDown) {
        dirty = false;
        std::ostringstream oss;
        oss << "FirstPerson: " << (g_firstPersonEnabled.load() ? "ON" : "off")
            << " distance=" << g_firstPersonDistance.load() << " m height=" << g_firstPersonHeight.load()
            << " m side=" << g_firstPersonSide.load() << " m";
        modlog::Line(oss.str());
    }
}

void PollCameraTraceHotkey() {
    static bool wasDown = false;
    bool isDown = (VRMOD_DEV_KEY(VK_F6) & 0x8000) != 0;
    bool pressed = isDown && !wasDown;
    wasDown = isDown;
    if (pressed && !g_traceArmed.load() && !g_traceNeedsFlush.load()) {
        g_traceCount.store(0);
        g_traceArmed.store(true);
        modlog::Line("CamTrace: armed, recording the next 128 camera writes.");
    }
}

void FlushCameraTraceIfReady() {
    if (!g_traceNeedsFlush.exchange(false)) {
        return;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    modlog::Line("CamTrace: flushing 128 writes (pre-patch, as the game wrote them).");
    for (int i = 0; i < kTraceMax; ++i) {
        const TraceEntry& e = g_trace[i];
        double t = static_cast<double>(e.qpc - g_trace[0].qpc) * 1000.0 / static_cast<double>(freq.QuadPart);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3)
            << "CAMTRACE " << i << " t=" << t << "ms frame=" << e.frame
            << " pos=(" << e.pos[0] << "," << e.pos[1] << "," << e.pos[2] << ")";
        if (i > 0) {
            const TraceEntry& p = g_trace[i - 1];
            float dx = e.pos[0] - p.pos[0], dy = e.pos[1] - p.pos[1], dz = e.pos[2] - p.pos[2];
            oss << " d=(" << dx << "," << dy << "," << dz << ")"
                << " |d|=" << std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        oss << " quat=(" << e.quat[0] << "," << e.quat[1] << "," << e.quat[2] << "," << e.quat[3] << ")"
            << " fov=" << e.fov;
        modlog::Line(oss.str());
    }
}

} // namespace vrmod

extern "C" void ApplyAimInjection(void* dst, void* src, unsigned long long returnAddress) {
    vrmod::RecordAimCaller(returnAddress);
    const uintptr_t d = reinterpret_cast<uintptr_t>(dst), sp = reinterpret_cast<uintptr_t>(src);
    if (returnAddress == vrmod::g_aimInjectRets[0]) vrmod::g_cameraComponent.store(d - 0x2B0, std::memory_order_relaxed);
    if (returnAddress == vrmod::g_aimInjectRets[1]) vrmod::g_comp450.store(d, std::memory_order_relaxed);
    if (returnAddress == vrmod::g_aimInjectRets[2]) vrmod::g_comp930.store(d, std::memory_order_relaxed);
    if (vrmod::g_headAim.load(std::memory_order_relaxed)) {
        if (vrmod::g_headAimSuspended.load(std::memory_order_relaxed)) return;
        if (vrmod::HeadDriveActive()) {
            // Head drive turns the game camera itself, so no rotation here -- but the aim records still start at the eye.
            for (int k = 1; k <= 2; ++k) {
                if (vrmod::g_aimInjectRets[k] && returnAddress == vrmod::g_aimInjectRets[k]) { vrmod::HeadAimMoveToEye(k, d); break; }
            }
            return;
        }
        for (int k = 0; k < 4; ++k) {
            if (vrmod::g_aimInjectRets[k] && returnAddress == vrmod::g_aimInjectRets[k]) { vrmod::HeadAimInject(k, d, sp); break; }
        }
        return;
    }
    const int mode = vrmod::g_aimInjectMode.load(std::memory_order_relaxed);
    if (mode == 2) {
        const uintptr_t c450 = vrmod::g_comp450.load(std::memory_order_relaxed), c930 = vrmod::g_comp930.load(std::memory_order_relaxed);
        if (sp && (sp == c450 || sp == c930) && returnAddress != vrmod::g_prevCopyRet[0] && returnAddress != vrmod::g_prevCopyRet[1]) {
            for (vrmod::CallerSlot& c : vrmod::g_slotInjectCallers) {
                uint64_t cur = c.ret.load(std::memory_order_relaxed);
                if (cur == 0) {
                    uint64_t expected = 0;
                    cur = c.ret.compare_exchange_strong(expected, returnAddress) ? returnAddress : expected;
                }
                if (cur == returnAddress) { c.count.fetch_add(1, std::memory_order_relaxed); break; }
            }
            vrmod::InjectYaw(d, 0);
        }
    } else if (mode == 1) {
        for (uintptr_t r : vrmod::g_aimInjectRets) {
            if (r && returnAddress == r) { vrmod::InjectYaw(d, reinterpret_cast<uintptr_t>(src)); break; }
        }
    }
}
