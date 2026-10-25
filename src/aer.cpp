#include "build_config.h"
#include "aer.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "audio_cue.h"
#include "logger.h"
#include "perf_stats.h"
#include "settings.h"

namespace vrmod {

namespace {

struct Q { float x, y, z, w; };
struct V { float x, y, z; };

Q Mul(const Q& a, const Q& b) {   // Hamilton product: b applied first, then a
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
Q Conj(const Q& q) { return { -q.x, -q.y, -q.z, q.w }; }
V Rot(const Q& q, const V& v) {   // q v q*
    const V t{ 2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z), 2.0f * (q.x * v.y - q.y * v.x) };
    return { v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
             v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
             v.z + q.w * t.z + (q.x * t.y - q.y * t.x) };
}
Q ToQ(const float* q) { return { q[0], q[1], q[2], q[3] }; }
V ToV(const float* p) { return { p[0], p[1], p[2] }; }
// OpenXR LOCAL (x right, y up, z back) -> game camera-local (X right, Y forward, Z up).
V MapV(const V& v) { return { v.x, -v.z, v.y }; }
Q MapQ(const Q& q) { return { q.x, -q.z, q.y, q.w }; }

PoseF Centre(const PoseF& a, const PoseF& b) {   // midpoint, mean orientation (same hemisphere, normalised)
    const float s = (a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3]) < 0.0f ? -1.0f : 1.0f;
    PoseF c{};
    float n = 0.0f;
    for (int i = 0; i < 4; ++i) { c.q[i] = a.q[i] + s * b.q[i]; n += c.q[i] * c.q[i]; }
    n = std::sqrt(n);
    for (int i = 0; i < 4; ++i) c.q[i] = n > 0.0f ? c.q[i] / n : (i == 3 ? 1.0f : 0.0f);
    for (int i = 0; i < 3; ++i) c.p[i] = 0.5f * (a.p[i] + b.p[i]);
    return c;
}

Q YawOnly(const Q& q) {   // OpenXR: yaw about +Y
    const float yaw = std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
}

constexpr int kRing = 256;
struct FrameRec {
    uint64_t frame = UINT64_MAX;
    int eye = 0;
    PoseF pose{};     // the eye's absolute OpenXR pose used for this frame (declared at submit)
    PoseF poseOther{};   // the other eye's pose at the same moment (AFW: its image is warped from this frame)
    Q deltaLocal{ 0, 0, 0, 1 };
    V offsetLocal{ 0, 0, 0 };   // game camera-local, game units (cm)
    float fov = 0.0f;           // FOV value written into the struct
    int64_t targetTimeNs = 0;   // display time the pose was predicted for
    bool mono = false;          // rendered from the head centre (Phase 8 mono render)
    bool haveFinal = false;     // the game's final projection for this frame (AerRecordFinalProjection)
    float finalFov = 0.0f, finalAspect = 0.0f;
};

std::mutex g_mutex;
PoseF g_views[2];
int64_t g_viewsTargetNs = 0;
bool g_haveViews = false;
bool g_recenter = true;
Q g_qRef{ 0, 0, 0, 1 };
V g_pRef{ 0, 0, 0 };
FrameRec g_ring[kRing];
float g_lastBase[4] = { 0, 0, 0, 1 };
std::atomic<float> g_worldScale{ 1.0f };
std::atomic<float> g_gameFov{ 0.0f };   // 0 = not set yet (the hook then keeps head tracking's FOV)
bool g_probeFreeze = false;             // guarded by g_mutex
PoseF g_probeViews[2];
float g_probeYaw = 0.0f, g_probePitch = 0.0f;
std::atomic<bool> g_holdBase{ false };
std::atomic<bool> g_monoRender{ false };
bool g_scaleLoaded = false;

// Axis log (Ctrl+X).
struct AxisLog {
    bool active = false;
    double startMs = 0.0, lastMs = 0.0;
    std::ostringstream rows;
};
AxisLog g_axis;

float Deg(float r) { return r * 57.2957795f; }

void RecenterLocked() {
    const Q q0 = ToQ(g_views[0].q);
    g_qRef = YawOnly(q0);
    g_pRef = { 0.5f * (g_views[0].p[0] + g_views[1].p[0]), 0.5f * (g_views[0].p[1] + g_views[1].p[1]),
               0.5f * (g_views[0].p[2] + g_views[1].p[2]) };
    g_recenter = false;
    modlog::Line("AER: recentred (yaw-only orientation reference; position reference = midpoint between the eyes).");
}

// The eye's pose relative to the recentre reference, in game camera-local axes.
void Relative(const PoseF& eye, Q* delta, V* offsetCm) {
    const Q qi = Conj(g_qRef);
    const Q qrel = Mul(qi, ToQ(eye.q));
    const V prel = Rot(qi, { eye.p[0] - g_pRef.x, eye.p[1] - g_pRef.y, eye.p[2] - g_pRef.z });
    *delta = MapQ(qrel);
    const V m = MapV(prel);
    const float s = 100.0f * g_worldScale.load(std::memory_order_relaxed);   // game units are cm
    *offsetCm = { m.x * s, m.y * s, m.z * s };
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void AxisLogSample() {
    if (!g_axis.active) return;
    const double now = NowMs();
    if (now - g_axis.lastMs < 50.0) return;
    g_axis.lastMs = now;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_haveViews) return;
    // Head = left eye's orientation, midpoint position (as the recentre reference).
    PoseF head = g_views[0];
    for (int i = 0; i < 3; ++i) head.p[i] = 0.5f * (g_views[0].p[i] + g_views[1].p[i]);
    const Q qrel = Mul(Conj(g_qRef), ToQ(head.q));
    const V prel = Rot(Conj(g_qRef), { head.p[0] - g_pRef.x, head.p[1] - g_pRef.y, head.p[2] - g_pRef.z });
    // OpenXR angles: yaw left +, pitch up +, roll right (right ear down) +.
    const V f = Rot(qrel, { 0, 0, -1 }), r = Rot(qrel, { 1, 0, 0 });
    const float xrYaw = Deg(std::atan2(-f.x, -f.z)), xrPitch = Deg(std::asin((std::max)(-1.0f, (std::min)(1.0f, f.y)))),
                xrRoll = Deg(-std::asin((std::max)(-1.0f, (std::min)(1.0f, r.y))));
    // The same, after mapping into the game camera's local axes (X right, Y forward, Z up).
    Q d;
    V off;
    Relative(head, &d, &off);
    const V F = Rot(d, { 0, 1, 0 }), R = Rot(d, { 1, 0, 0 });
    const float gYaw = Deg(std::atan2(-F.x, F.y)), gPitch = Deg(std::asin((std::max)(-1.0f, (std::min)(1.0f, F.z)))),
                gRoll = Deg(-std::asin((std::max)(-1.0f, (std::min)(1.0f, R.z))));
    // The game's base camera forward in world space (mouse / stick turning shows the world's up axis).
    const V bf = Rot(ToQ(g_lastBase), { 0, 1, 0 });
    // Eye views: separation (IPD) and the angle between their orientations (are they parallel?).
    const float dx = g_views[1].p[0] - g_views[0].p[0], dy = g_views[1].p[1] - g_views[0].p[1], dz = g_views[1].p[2] - g_views[0].p[2];
    const Q q0 = ToQ(g_views[0].q), q1 = ToQ(g_views[1].q);
    float dot = std::fabs(q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w);
    dot = (std::min)(1.0f, dot);
    char line[512];
    snprintf(line, sizeof(line), "%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.1f,%.3f\n",
        (now - g_axis.startMs) / 1000.0, xrYaw, xrPitch, xrRoll, prel.x, prel.y, prel.z, gYaw, gPitch, gRoll,
        off.x, off.y, off.z, bf.x, bf.y, bf.z, 1000.0f * std::sqrt(dx * dx + dy * dy + dz * dz), Deg(2.0f * std::acos(dot)));
    g_axis.rows << line;
}

bool Edge(int vk, bool ctrl, bool& was) {
    const bool d = (VRMOD_DEV_KEY(vk) & 0x8000) != 0;
    const bool pressed = d && !was;
    was = d;
    if (!pressed) return false;
    const bool c = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, a = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return ctrl ? (c && !a) : (!c && !a);
}

} // namespace

void AerPublishViews(const PoseF views[2], int64_t targetTimeNs) {
    if (!g_scaleLoaded) {
        g_scaleLoaded = true;
        g_worldScale.store(GetRenderWorldScale());
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_views[0] = views[0];
    g_views[1] = views[1];
    g_viewsTargetNs = targetTimeNs;
    g_haveViews = true;
    if (g_recenter) RecenterLocked();
}

void AerRequestRecenter(const char* why) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_recenter = true;
    modlog::Line(std::string("AER: recentre requested (") + why + ")");
}

void AerPollHotkeys() {
    static bool f8 = false, lb = false, rb = false, h = false, x = false;
    if (Edge(VK_F8, false, f8)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_recenter = true;
    }
    const bool down = Edge(VK_OEM_4, true, lb);   // Ctrl+[
    const bool up = Edge(VK_OEM_6, true, rb);     // Ctrl+]
    if (down || up) {
        const float s = g_worldScale.load() * (up ? 1.05f : 1.0f / 1.05f);
        g_worldScale.store(s);
        SetRenderWorldScaleForSave(s);
        char b[96];
        snprintf(b, sizeof(b), "AER: world scale %.3f (Ctrl+Alt+0 saves it)", s);
        modlog::Line(b);
        PlayCue(up ? Cue::On : Cue::Off);
    }
    if (Edge('H', true, h)) {
        const bool on = !g_holdBase.load();
        g_holdBase.store(on);
        modlog::Line(std::string("AER: base camera ") + (on ? "HELD (only head motion moves the view)" : "released"));
        PlayCue(on ? Cue::On : Cue::Off);
    }
    if (Edge('X', true, x) && !g_axis.active) {
        g_axis = AxisLog{};
        g_axis.active = true;
        g_axis.startMs = NowMs();
        g_axis.rows << "t_s,xr_yaw_left_deg,xr_pitch_up_deg,xr_roll_right_deg,xr_x_right_m,xr_y_up_m,xr_z_back_m,"
                       "game_yaw_left_deg,game_pitch_up_deg,game_roll_right_deg,game_right_cm,game_forward_cm,game_up_cm,"
                       "base_forward_world_x,base_forward_world_y,base_forward_world_z,eye_separation_mm,eye_orientation_diff_deg\n";
        modlog::Line("AER: axis log started (20 s): look left, look up, tilt head right, move head right, up, forward -- "
                     "returning to centre after each -- then turn the game camera with the mouse.");
        PlayCue(Cue::On);
    }
    AxisLogSample();
    if (g_axis.active && NowMs() - g_axis.startMs > 20000.0) {
        g_axis.active = false;
        const std::string name = "vrmod_axes_" + TimeStamp() + ".csv";
        std::ofstream(name, std::ios::binary) << g_axis.rows.str();
        modlog::Line("AER: axis log written to " + name);
        PlayCue(Cue::Success);
    }
}

bool AerComputeCamera(uint64_t gameFrame, const float baseQuat[4], float outDelta[4], float outOffsetWorld[3], float* outFov) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_haveViews) return false;
    for (int i = 0; i < 4; ++i) g_lastBase[i] = baseQuat[i];
    FrameRec& r = g_ring[gameFrame % kRing];
    if (r.frame != gameFrame) {   // first write of this game frame: choose the eye and fix its pose
        r.frame = gameFrame;
        r.haveFinal = false;
        r.eye = g_probeFreeze ? 0 : static_cast<int>(gameFrame & 1);
        r.mono = !g_probeFreeze && g_monoRender.load(std::memory_order_relaxed);
        r.pose = g_probeFreeze ? g_probeViews[0] : r.mono ? Centre(g_views[0], g_views[1]) : g_views[r.eye];
        r.poseOther = g_probeFreeze ? g_probeViews[0] : r.mono ? r.pose : g_views[1 - r.eye];
        Relative(r.pose, &r.deltaLocal, &r.offsetLocal);
        if (g_probeFreeze && (g_probeYaw != 0.0f || g_probePitch != 0.0f)) {
            // Extra camera-local turn: yaw about +Z (up), then pitch about +X (right).
            const Q yaw{ 0, 0, std::sin(0.5f * g_probeYaw), std::cos(0.5f * g_probeYaw) };
            const Q pitch{ std::sin(0.5f * g_probePitch), 0, 0, std::cos(0.5f * g_probePitch) };
            r.deltaLocal = Mul(Mul(r.deltaLocal, yaw), pitch);
        }
        r.fov = g_gameFov.load(std::memory_order_relaxed);
        r.targetTimeNs = g_viewsTargetNs;
    }
    *outFov = r.fov;
    outDelta[0] = r.deltaLocal.x;
    outDelta[1] = r.deltaLocal.y;
    outDelta[2] = r.deltaLocal.z;
    outDelta[3] = r.deltaLocal.w;
    const V w = Rot(ToQ(baseQuat), r.offsetLocal);
    outOffsetWorld[0] = w.x;
    outOffsetWorld[1] = w.y;
    outOffsetWorld[2] = w.z;
    return true;
}

bool AerHeadAimDelta(float outDelta[4]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_haveViews) return false;
    Q d;
    V off;
    Relative(Centre(g_views[0], g_views[1]), &d, &off);
    // Forward (+Y) of the head in camera-local axes; Rz(yaw) * Rx(pitch) * (0,1,0) = (-sin y cos p, cos y cos p, sin p).
    const V f = Rot(d, { 0.0f, 1.0f, 0.0f });
    const float yaw = std::atan2(-f.x, f.y);
    const float pitch = std::asin(f.z < -1.0f ? -1.0f : f.z > 1.0f ? 1.0f : f.z);
    const Q qz{ 0.0f, 0.0f, std::sin(0.5f * yaw), std::cos(0.5f * yaw) };
    const Q qx{ std::sin(0.5f * pitch), 0.0f, 0.0f, std::cos(0.5f * pitch) };
    const Q r = Mul(qz, qx);
    outDelta[0] = r.x;
    outDelta[1] = r.y;
    outDelta[2] = r.z;
    outDelta[3] = r.w;
    return true;
}

std::atomic<uint64_t> g_lastPairedFrame{0};   // the game frame the latest Present was paired with (DLSS probe)

uint64_t AerLastPairedFrame() {
    return g_lastPairedFrame.load(std::memory_order_relaxed);
}

int AerEyeOfFrame(uint64_t gameFrame) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const FrameRec& r = g_ring[gameFrame % kRing];
    return r.frame == gameFrame ? r.eye : -1;
}

bool AerHoldBase() {
    return g_holdBase.load(std::memory_order_relaxed);
}

void AerSetHoldBase(bool on) {
    g_holdBase.store(on, std::memory_order_relaxed);
}

std::atomic<int> g_pairShiftReq{0};
std::atomic<int> g_pairExtraLag{0};   // written by the Present thread only

void AerRequestPairShift(int frames) {
    g_pairShiftReq.fetch_add(frames, std::memory_order_relaxed);
}

int AerPairExtraLag() {
    return g_pairExtraLag.load(std::memory_order_relaxed);
}

bool AerPairPresent(uint64_t latestGameFrame, uint64_t present, AerPresented* out) {
    static long long S = -1;
    static uint64_t lastPresent = 0;
    const long long G = static_cast<long long>(latestGameFrame);
    if (const int req = g_pairShiftReq.exchange(0, std::memory_order_relaxed)) {
        int lag = g_pairExtraLag.load(std::memory_order_relaxed) - req;
        lag = lag < -1 ? -1 : (lag > 2 ? 2 : lag);
        g_pairExtraLag.store(lag, std::memory_order_relaxed);
        if (S >= 0) S += req;
    }
    const long long extra = g_pairExtraLag.load(std::memory_order_relaxed);
    const long long lo = G - 2 - extra, hi = G - 1 - extra;
    const bool restart = S < 0 || present <= lastPresent || present - lastPresent > 8;
    long long pred = restart ? lo : S + static_cast<long long>(present - lastPresent);
    bool corrected = false;
    if (pred < lo) { pred = lo; corrected = !restart; }
    if (pred > hi) { pred = hi; corrected = !restart; }
    S = pred;
    lastPresent = present;
    if (S < 0) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const FrameRec& r = g_ring[static_cast<uint64_t>(S) % kRing];
    if (r.frame != static_cast<uint64_t>(S)) return false;
    out->eye = r.eye;
    g_lastPairedFrame.store(r.frame, std::memory_order_relaxed);
    out->pose = r.pose;
    out->poseOther = r.poseOther;
    out->gameFrame = r.frame;
    out->fov = r.fov;
    out->targetTimeNs = r.targetTimeNs;
    out->corrected = corrected;
    out->mono = r.mono;
    out->haveFinal = r.haveFinal;
    out->finalFov = r.finalFov;
    out->finalAspect = r.finalAspect;
    return true;
}

void AerSetGameFov(float fov) {
    g_gameFov.store(fov, std::memory_order_relaxed);
}

void AerProbeSet(bool freeze, float yaw, float pitch) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (freeze && !g_probeFreeze) {
        g_probeViews[0] = g_views[0];
        g_probeViews[1] = g_views[1];
    }
    g_probeFreeze = freeze;
    g_probeYaw = freeze ? yaw : 0.0f;
    g_probePitch = freeze ? pitch : 0.0f;
}

float AerWorldScale() {
    return g_worldScale.load(std::memory_order_relaxed);
}

void AerSetWorldScale(float scale) {
    if (scale < 0.05f || scale > 20.0f) return;
    g_worldScale.store(scale);
    SetRenderWorldScaleForSave(scale);
}

void AerRecordFinalProjection(uint64_t gameFrame, float fovV, float aspect) {
    std::lock_guard<std::mutex> lock(g_mutex);
    FrameRec& r = g_ring[gameFrame % kRing];
    if (r.frame != gameFrame) return;
    r.finalFov = fovV;
    r.finalAspect = aspect;
    r.haveFinal = true;
}

bool AerFinalProjectionOf(uint64_t gameFrame, float* fovV, float* aspect) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const FrameRec& r = g_ring[gameFrame % kRing];
    if (r.frame != gameFrame || !r.haveFinal) return false;
    *fovV = r.finalFov;
    *aspect = r.finalAspect;
    return true;
}

void AerSetMonoRender(bool on) {
    g_monoRender.store(on, std::memory_order_relaxed);
}

} // namespace vrmod
