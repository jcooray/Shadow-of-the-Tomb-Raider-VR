#include "build_config.h"
#include "head_tracking.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <sstream>

#include "logger.h"
#include "camera_interceptor.h"
#include "audio_cue.h"

namespace vrmod {

namespace {

constexpr int kRing = 64;
constexpr float kDefaultGameFov = 1.6f; // ~92 deg vertical: fills the headset vertically

struct Quat {
    float x, y, z, w;
};

// Hamilton product a * b: b is applied first, then a.
Quat Mul(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat Conj(const Quat& q) {
    return { -q.x, -q.y, -q.z, q.w };
}

std::mutex g_mutex;
HeadSample g_ring[kRing];
uint32_t g_seq = 0;
uint32_t g_usedSeq[kRing] = {};
uint64_t g_usedFrame[kRing] = {};
uint64_t g_presentFrame = 0;

Quat g_ref{ 0.0f, 0.0f, 0.0f, 1.0f };
bool g_freeze = false;          // HeadTrackingFreeze: keep giving the game the delta from the moment it was set
bool g_haveFrozen = false;
Quat g_frozen{ 0.0f, 0.0f, 0.0f, 1.0f };
bool g_recenterRequested = true;
bool g_enabledOnce = false;

// 2, not 1: a single `=` press fixed head-tracking judder in the sub-72 fps
// areas, and 2 also held up in the 72 fps area. The camera-write trace shows
// the same structure in both (exactly 2 identical writes per frame, ~0.1 ms
// apart) -- only the pacing differs, so the default of 1 was simply too low.
int g_latencyFrames = 3;   // real frames; the tuned default comes from vrmod_settings.ini (SetLatencyDefault)
int g_latencyDefault = 3;
float g_gameFovDefault = kDefaultGameFov;
float g_gameFov = kDefaultGameFov;

// Yaw-only reference: recentring keeps gravity, so a head that's pitched at
// recentre time isn't baked in as a permanent pitch offset.
Quat YawOnlyReference(const Quat& q) {
    float yaw = std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
}

// ---- frame-number ("ordinal") matching ----
// The game writes its camera twice per frame, so write index / 2 is a frame ordinal k. The frame with
// ordinal k is later shown by one specific real Present, P = k - c for a constant alignment c. Matching
// the presented image to the write with the same ordinal removes any dependence on how far ahead the
// game's main thread happened to be (the pipeline delay in frames), which the fixed-latency lookup
// assumes never changes. c is derived from the running mean of D = k - (presents so far at the write),
// which is what the fixed latency describes on average: c = mean(D) - latencyFrames.
uint64_t g_writeCount = 0;
uint32_t g_ordSeq[kRing] = {};
uint64_t g_ordTag[kRing] = {};
bool g_useOrdinal = false;
int64_t g_ordinalC = 0;
uint64_t g_ordinalMisses = 0;
uint64_t g_ordinalUsed = 0;
// Running lead statistics D = k - g_presentFrame (min / sum / max over the current 600-present window).
int64_t g_leadMin = INT64_MAX, g_leadMax = INT64_MIN;
double g_leadSum = 0.0;
uint64_t g_leadN = 0;
double g_leadMeanLast = 0.0;   // mean of the last completed window (or the live one until the first completes)
bool g_ordinalWanted = true;   // default on; the startup default is read from vrmod_settings.ini

// Automatic realignment. The alignment is c = round(g_refD) - latencyFrames, where g_refD is the mean write
// lead D at alignment time (so latency trims and calibration take effect live). A rolling window of the
// last 90 per-frame D values is compared with g_refD every 15 frames; when it has moved by 0.75 or more
// (frames dropped/repeated, a cutscene that wrote no camera, ...) g_refD follows it. Nothing switches to
// the fixed-latency lookup meanwhile: the old alignment stays in use until the new one is known, which
// takes about 60-90 frames after a break instead of a whole 600-frame window.
constexpr int kRollN = 90;
int64_t g_roll[kRollN] = {};
int g_rollCount = 0;
int g_rollPos = 0;
int g_holdoffFrames = 0;
double g_refD = 0.0;
double g_lastAdvanceMs = 0.0;

int64_t CurrentOrdinalC() {
    return static_cast<int64_t>(std::llround(g_refD)) - static_cast<int64_t>(g_latencyFrames);
}

void ClearRollLocked() {
    g_rollCount = 0;
    g_rollPos = 0;
}

double NowMsHT() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// Caller holds g_mutex.
void MarkDiscontinuityLocked(const char* why) {
    if (!g_useOrdinal) {
        return;
    }
    // Start collecting fresh lead samples after a short settling period; the current alignment stays in use.
    ClearRollLocked();
    g_holdoffFrames = 15;
    modlog::Line(std::string("HeadTracking: alignment may shift (") + why + "); re-checking over the next ~60-90 frames, keeping the current alignment meanwhile.");
}

// Caller holds g_mutex. Aligns on the given mean write lead; the lookup then follows the per-frame variation.
void EnableOrdinalLocked(double meanLead, const char* why) {
    g_refD = meanLead;
    g_useOrdinal = true;
    ClearRollLocked();
    g_holdoffFrames = 0;
    g_ordinalUsed = g_ordinalMisses = 0;
    std::ostringstream oss;
    oss << "HeadTracking: ordinal (frame-number) matching ON [" << why << "], alignment c=" << CurrentOrdinalC()
        << " (mean write lead " << meanLead << " - latencyFrames " << g_latencyFrames << ")";
    modlog::Line(oss.str());
}

// ---- latency calibration state (see HeadTrackingStartLatencyCalibration) ----
bool g_calActive = false;
uint64_t g_calStartFrame = 0;
int g_calFrames = 0;
constexpr float kCalAmplitude = 0.0105f;   // ~0.6 degrees

// Deterministic +/- amplitude per present interval; zero outside the window. Caller holds g_mutex
// or is otherwise serialised (the fields are only changed at start/finish).
float CalOffsetLocked(uint64_t frame) {
    if (!g_calActive || frame < g_calStartFrame || frame >= g_calStartFrame + static_cast<uint64_t>(g_calFrames)) {
        return 0.0f;
    }
    uint64_t x = (frame - g_calStartFrame) * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return (x & 1) ? kCalAmplitude : -kCalAmplitude;
}

void LogTuning(const char* why) {
    std::ostringstream oss;
    oss << "HeadTracking: " << why << " | latencyFrames=" << g_latencyFrames
        << " gameFov=" << g_gameFov;
    modlog::Line(oss.str());
}

} // namespace

void HeadTrackingPublish(const float orientation[4], const float eyePos0[3], const float eyePos1[3]) {
    bool firstEver = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_seq;
        HeadSample& s = g_ring[g_seq % kRing];
        s.tMs = NowMsHT();
        for (int i = 0; i < 4; ++i) s.q[i] = orientation[i];
        for (int i = 0; i < 3; ++i) {
            s.eyePos[0][i] = eyePos0[i];
            s.eyePos[1][i] = eyePos1[i];
        }

        if (g_recenterRequested) {
            g_ref = YawOnlyReference({ orientation[0], orientation[1], orientation[2], orientation[3] });
            g_recenterRequested = false;
            modlog::Line("HeadTracking: recentred (yaw reference captured).");
        }

        if (!g_enabledOnce) {
            g_enabledOnce = true;
            firstEver = true;
        }
    }

    if (firstEver) {
        SetHeadTrackingEnabled(true);
        LogTuning("first pose published, camera override ON (F11 toggles)");
    }
}

void HeadTrackingFreeze(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_freeze = on;
    g_haveFrozen = false;
}

bool HeadTrackingComputeGameDelta(float outQuat[4]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_seq == 0) {
        return false;
    }

    const HeadSample& s = g_ring[g_seq % kRing];
    Quat now{ s.q[0], s.q[1], s.q[2], s.q[3] };
    Quat delta = Mul(Conj(g_ref), now);

    // OpenXR (x right, y up, z back) -> game camera-local (X right, Y forward,
    // Z up): (x, y, z) -> (x, -z, y). Proper rotation, so the quaternion's
    // vector part maps the same way.
    Quat g{ delta.x, -delta.z, delta.y, delta.w };

    // Frozen (ghost test): the camera must not follow the head while frames are compared.
    if (g_freeze) {
        if (!g_haveFrozen) {
            g_frozen = g;
            g_haveFrozen = true;
        }
        g = g_frozen;
    }

    // Latency calibration: an extra yaw (rotation about the camera's own up axis) that
    // is constant within a present interval and pseudo-random between intervals.
    if (g_calActive) {
        const float a = CalOffsetLocked(g_presentFrame);
        if (a != 0.0f) {
            g = Mul(g, Quat{ 0.0f, 0.0f, std::sin(a * 0.5f), std::cos(a * 0.5f) });
        }
    }

    outQuat[0] = g.x;
    outQuat[1] = g.y;
    outQuat[2] = g.z;
    outQuat[3] = g.w;

    size_t i = g_presentFrame % kRing;
    g_usedSeq[i] = g_seq;
    g_usedFrame[i] = g_presentFrame;

    // Frame ordinal bookkeeping (two writes per frame -> ordinal = write index / 2).
    const uint64_t k = g_writeCount / 2;
    ++g_writeCount;
    g_ordSeq[k % kRing] = g_seq;
    g_ordTag[k % kRing] = k;
    const int64_t lead = static_cast<int64_t>(k) - static_cast<int64_t>(g_presentFrame);
    if (g_writeCount % 2 == 1 && g_holdoffFrames == 0) {   // first write of each frame -> one rolling sample per frame
        g_roll[g_rollPos] = lead;
        g_rollPos = (g_rollPos + 1) % kRollN;
        if (g_rollCount < kRollN) ++g_rollCount;
    }
    if (lead < g_leadMin) g_leadMin = lead;
    if (lead > g_leadMax) g_leadMax = lead;
    g_leadSum += static_cast<double>(lead);
    ++g_leadN;
    return true;
}

bool HeadTrackingGetSampleForSubmit(HeadSample* out) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_useOrdinal) {
        const int64_t k = static_cast<int64_t>(g_presentFrame) + CurrentOrdinalC();
        if (k >= 0) {
            const uint64_t ku = static_cast<uint64_t>(k);
            const size_t i = ku % kRing;
            if (g_ordTag[i] == ku && (g_seq - g_ordSeq[i]) < kRing) {
                *out = g_ring[g_ordSeq[i] % kRing];
                ++g_ordinalUsed;
                return true;
            }
        }
        ++g_ordinalMisses;   // not written yet or too old: fall through to the fixed-latency lookup
    }

    uint64_t target = (g_presentFrame >= static_cast<uint64_t>(g_latencyFrames))
        ? g_presentFrame - g_latencyFrames : 0;

    // Newest camera write at or before the target interval. During a frame
    // drop an interval can have no write at all (the game thread stalled, the
    // render side re-presents); that image was still rendered with the last
    // pose written, so walk back to it. Falling back to the *live* pose there
    // is wrong -- it tells the compositor the image was rendered for a pose
    // it wasn't, and the "correction" shows up as head-tracking judder.
    for (uint64_t back = 0; back < 8 && back <= target; ++back) {
        uint64_t frame = target - back;
        size_t i = frame % kRing;
        if (g_usedFrame[i] == frame && g_usedSeq[i] != 0 && (g_seq - g_usedSeq[i]) < kRing) {
            *out = g_ring[g_usedSeq[i] % kRing];
            return true;
        }
    }
    return false;
}

uint64_t HeadTrackingGetPresentFrame() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_presentFrame;
}

void HeadTrackingAdvanceFrame() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_presentFrame;

    // A long gap between real Presents (loading, a hitch) can leave the write/Present counts out of step.
    {
        const double now = NowMsHT();
        if (g_lastAdvanceMs > 0.0 && now - g_lastAdvanceMs > 300.0) {
            MarkDiscontinuityLocked(("stall of " + std::to_string(static_cast<int>(now - g_lastAdvanceMs)) + " ms").c_str());
        }
        g_lastAdvanceMs = now;
    }

    if (g_holdoffFrames > 0) {
        --g_holdoffFrames;
    }

    // Quick alignment / realignment from the rolling window (needs 60 clean per-frame samples; checked every
    // 15 frames): switches matching on shortly after start, and follows a shifted lead within ~2 seconds.
    if (g_holdoffFrames == 0 && g_rollCount >= 60 && g_presentFrame % 15 == 0) {
        double sum = 0.0;
        for (int i = 0; i < g_rollCount; ++i) sum += static_cast<double>(g_roll[i]);
        const double mean = sum / static_cast<double>(g_rollCount);
        if (g_ordinalWanted && !g_useOrdinal) {
            EnableOrdinalLocked(mean, "auto (default on)");   // quiet: no cue
        } else if (g_useOrdinal && std::fabs(mean - g_refD) >= 0.75) {
            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss.precision(2);
            oss << "HeadTracking: auto realign, write lead moved from " << g_refD << " to " << mean
                << " (c " << CurrentOrdinalC() << " -> " << (static_cast<int64_t>(std::llround(mean)) - g_latencyFrames) << ")";
            modlog::Line(oss.str());
            g_refD = mean;
            ClearRollLocked();
        }
    }

    // Every 600 real frames: report how far ahead of the Presents the game's writes run (D = k - presents
    // at the write). A constant D means a steady pipeline; a wide min..max means the delay wobbles.
    if (g_presentFrame % 600 == 0 && g_leadN > 0) {
        g_leadMeanLast = g_leadSum / static_cast<double>(g_leadN);
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(2);
        oss << "HeadTracking: write lead D over the last 600 frames: min " << g_leadMin << " mean " << g_leadMeanLast
            << " max " << g_leadMax << " (spread " << (g_leadMax - g_leadMin) << ")"
            << " | latencyFrames=" << g_latencyFrames << " | ordinal matching " << (g_useOrdinal ? "ON" : "off");
        if (g_useOrdinal) {
            oss << " c=" << CurrentOrdinalC() << " refD=" << g_refD << " used=" << g_ordinalUsed << " misses=" << g_ordinalMisses;
        }
        modlog::Line(oss.str());
        g_leadMin = INT64_MAX;
        g_leadMax = INT64_MIN;
        g_leadSum = 0.0;
        g_leadN = 0;
    }
}

void HeadTrackingMarkDiscontinuity(const char* why) {
    std::lock_guard<std::mutex> lock(g_mutex);
    MarkDiscontinuityLocked(why);
}

void HeadTrackingToggleOrdinalMatching() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_useOrdinal) {
        g_useOrdinal = false;
        g_ordinalWanted = false;
        modlog::Line("HeadTracking: ordinal matching off (fixed latency lookup).");
        PlayCue(Cue::Off);
    } else {
        g_ordinalWanted = true;
        // Align on the recent per-frame lead (rolling window), else the running statistics.
        double mean = g_leadN > 0 ? g_leadSum / static_cast<double>(g_leadN) : g_leadMeanLast;
        if (g_rollCount >= 10) {
            double sum = 0.0;
            for (int i = 0; i < g_rollCount; ++i) sum += static_cast<double>(g_roll[i]);
            mean = sum / static_cast<double>(g_rollCount);
        }
        EnableOrdinalLocked(mean, "toggle");
        PlayCue(Cue::On);
    }
}

void HeadTrackingSetOrdinalDefault(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ordinalWanted = on;
}

bool HeadTrackingOrdinalMatchingWanted() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_ordinalWanted;
}

float HeadTrackingGetGameFov() {
    return g_gameFov;
}

void HeadTrackingAdjustFov(float factor) {
    float f = g_gameFov * factor;
    if (f < 0.6f) f = 0.6f;
    if (f > 2.4f) f = 2.4f;
    g_gameFov = f;
}

void HeadTrackingStartLatencyCalibration(int frames) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_calStartFrame = g_presentFrame + 2;   // start on a fresh interval so no write straddles the boundary
    g_calFrames = frames;
    g_calActive = true;
    modlog::Line("LATCAL: started, " + std::to_string(frames) + " frames from present frame " + std::to_string(g_calStartFrame)
        + ". Hold your head still and look at a detailed, static scene.");
}

bool HeadTrackingCalibrating() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_calActive;
}

unsigned long long HeadTrackingCalStartFrame() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_calStartFrame;
}

int HeadTrackingCalFrames() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_calFrames;
}

float HeadTrackingCalOffset(unsigned long long frame) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return CalOffsetLocked(frame);
}

void HeadTrackingFinishCalibration() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_calActive = false;
}

void HeadTrackingApplyMeasuredLatency(int frames) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_latencyFrames = frames < 0 ? 0 : (frames > 8 ? 8 : frames);
    modlog::Line("LATCAL: applied latencyFrames=" + std::to_string(g_latencyFrames) + " (Ctrl+Alt+0 saves it as the startup default).");
}

void HeadTrackingSetLatencyDefault(int frames) {
    g_latencyDefault = frames < 0 ? 0 : (frames > 8 ? 8 : frames);
    g_latencyFrames = g_latencyDefault;
}

int HeadTrackingGetLatency() {
    return g_latencyFrames;
}

void HeadTrackingSetFovDefault(float fov) {
    g_gameFovDefault = fov < 0.6f ? 0.6f : (fov > 2.4f ? 2.4f : fov);
    g_gameFov = g_gameFovDefault;
}

void HeadTrackingResetTuning() {
    g_gameFov = g_gameFovDefault;
    g_latencyFrames = g_latencyDefault;
}

void HeadTrackingPollHotkeys() {
    auto down = [](int vk) {   // release build: only the modifiers are read (build_config.h)
        const bool real = vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT;
        return ((real ? GetAsyncKeyState(vk) : VRMOD_DEV_KEY(vk)) & 0x8000) != 0;
    };
    auto edge = [&](int vk, bool& wasDown) {
        bool d = down(vk);
        bool pressed = d && !wasDown;
        wasDown = d;
        return pressed;
    };

    static bool w8 = false, wm = false, wp = false, wo = false, wk = false;
    static int markCount = 0;

    // Ctrl+K: write a numbered MARK line to the log (with the time), so measurement phases can be told apart
    // afterwards and lined up with OptiScaler's log.
    if (edge('K', wk) && down(VK_CONTROL)) {
        ++markCount;
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[96];
        snprintf(buf, sizeof(buf), "MARK %d at %02d:%02d:%02d (write a note of what state you are in)", markCount, st.wHour, st.wMinute, st.wSecond);
        modlog::Line(buf);
        PlayCue(Cue::Success);
    }

    // Ctrl+M toggles frame-number matching (see HeadTrackingToggleOrdinalMatching).
    if (edge('M', wo) && down(VK_CONTROL)) {
        HeadTrackingToggleOrdinalMatching();
    }

    if (edge(VK_F8, w8)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_recenterRequested = true;
    }
    if (edge(VK_OEM_MINUS, wm)) {
        if (g_latencyFrames > 0) --g_latencyFrames;
        LogTuning("latency decreased");
    }
    if (edge(VK_OEM_PLUS, wp)) {
        if (g_latencyFrames < 8) ++g_latencyFrames;
        LogTuning("latency increased");
    }
}

} // namespace vrmod

namespace vrmod {
void HeadTrackingRequestRecenter() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_recenterRequested = true;
}
}
