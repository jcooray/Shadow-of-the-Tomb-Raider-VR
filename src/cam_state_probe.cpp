#include "cam_state_probe.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio_cue.h"
#include "camera_interceptor.h"
#include "logger.h"

namespace vrmod {

// Workstream A, the real fix (2026-10-02): find the game camera's own yaw/pitch state -- the value the right stick
// integrates -- so the head can drive the game camera itself. A0 (2026-09-25) walked pointers from the camera
// manager and found only copies; it never looked inside the camera controllers. This probe snapshots every
// controller in the camera component's blend stacks (entry +0x20; base entry +0xD20, lists +0xF00..+0xF98) and the
// component itself, each game frame for ~12 s after Ctrl+Alt+C, and correlates every float with the game camera's
// yaw (unwrapped) and pitch -- the game's rotation with head aim's d already taken off. The best matches go to the log
// with their slope (1 = radians, 57.3 = degrees, negative = opposite sign).
namespace {

constexpr int kMaxObjs = 24;
constexpr uint32_t kSpan = 0x1800;              // bytes per object
constexpr uint32_t kFloats = kSpan / 4;
constexpr int kFrames = 1800;   // camera writes (two per game frame): ~12.5 s

struct Acc { double n, s, ss, sy, sp; double dn, ds, dss, dsy, dsp; };   // values, and frame-to-frame changes
struct Obj {
    uintptr_t ptr = 0;
    std::string cls;
    std::vector<Acc> acc;
    double n = 0, sY = 0, sYY = 0, sP = 0, sPP = 0;   // targets over the frames this object was seen
    double dn = 0, dsY = 0, dsYY = 0, dsP = 0, dsPP = 0;   // target changes over consecutive samples
    double lastY = 0, lastP = 0;
    bool haveLast = false;
    std::vector<float> last;
};

Obj g_objs[kMaxObjs];
int g_nObjs = 0;
std::atomic<int> g_state{0};   // 0 idle, 1 armed (start next frame), 2 sampling, 3 done (report pending)
int g_frames = 0;
// Aim mode (Ctrl+Alt+C pressed while aiming): only camera writes with the aim camera dominating (stack-2 weight +0xC70
// > 0.5, i.e. the bow drawn) are sampled, until kFrames of them or 120 s -- the AimCameraController only takes over
// while the bow is drawn (holding aim alone leaves it at weight 0.02).
bool g_aimMode = false;
uintptr_t laraComps[16];
int nLara = -1;   // -1 = look Lara's components up at the next sample
ULONGLONG g_startMs = 0;
// Write test (Ctrl+Alt+E yaw +20 deg, Ctrl+Alt+I pitch 10 deg up): the ExplorationCameraController's +0x12D4/+0x12D8
// (yaw, radians) and +0x12EC/+0x12F0 (pitch, radians, negated) -- CAMSTATE 2026-10-02 01:37, slope 1.000 / -0.992.
// Applied once on the game thread; the game camera's yaw/pitch is logged before and 0.25 / 1 / 2 s after: a source
// keeps the turn, a copy snaps back.
std::atomic<int> g_kick{0};   // 1 yaw, 2 pitch
int g_kickWatch = -1;         // camera writes since the kick (-1 = not watching)
double g_kickYaw0 = 0, g_kickPitch0 = 0;
float g_kickVals0[4] = {};
double g_yawUnwrapped = 0.0, g_lastYaw = 0.0;
bool g_haveYaw = false;

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

bool CopyGuarded(uintptr_t src, void* dst, size_t n) {
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string ClassName(uintptr_t obj) {
    uintptr_t vt = 0, col = 0;
    int32_t tdRva = 0;
    if (!Rd(obj, &vt) || !Rd(vt - 8, &col) || !Rd(col + 0x0C, &tdRva)) return "?";
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char name[80] = {};
    if (!CopyGuarded(base + static_cast<uint32_t>(tdRva) + 0x10, name, sizeof(name) - 1)) return "?";
    std::string s(name);
    if (s.rfind(".?AV", 0) == 0) s = s.substr(4);
    const size_t at = s.find("@@");
    if (at != std::string::npos) s = s.substr(0, at);
    return s;
}

Obj* Slot(uintptr_t p) {
    for (int i = 0; i < g_nObjs; ++i) if (g_objs[i].ptr == p) return &g_objs[i];
    if (g_nObjs >= kMaxObjs) return nullptr;
    Obj& o = g_objs[g_nObjs++];
    o.ptr = p;
    o.cls = ClassName(p);
    o.acc.assign(kFloats, Acc{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
    o.last.assign(kFloats, 0.0f);
    return &o;
}

void Sample(uintptr_t p, double Y, double P) {
    Obj* o = Slot(p);
    if (!o) return;
    static float buf[kFloats];
    if (!CopyGuarded(p, buf, kSpan)) return;
    o->n += 1; o->sY += Y; o->sYY += Y * Y; o->sP += P; o->sPP += P * P;
    const bool delta = o->haveLast;
    const double dY = Y - o->lastY, dP = P - o->lastP;
    if (delta) { o->dn += 1; o->dsY += dY; o->dsYY += dY * dY; o->dsP += dP; o->dsPP += dP * dP; }
    for (uint32_t i = 0; i < kFloats; ++i) {
        const float v = buf[i];
        const float prev = o->last[i];
        o->last[i] = v;
        if (!std::isfinite(v) || std::fabs(v) > 1e5f) continue;
        Acc& a = o->acc[i];
        a.n += 1; a.s += v; a.ss += static_cast<double>(v) * v; a.sy += v * Y; a.sp += v * P;
        if (delta && std::isfinite(prev)) {
            double d = static_cast<double>(v) - prev;
            // An angle wrapping by a full turn (radians or degrees) is not a change.
            if (d > 3.14159265) d -= 6.2831853; else if (d < -3.14159265) d += 6.2831853;
            if (std::fabs(d) < 1.0) { a.dn += 1; a.ds += d; a.dss += d * d; a.dsy += d * dY; a.dsp += d * dP; }
        }
    }
    o->lastY = Y; o->lastP = P; o->haveLast = true;
}

struct Hit { double r, slope; int obj; uint32_t off; float last; };

void Report() {
    for (int t = 0; t < 2; ++t) {
        std::vector<Hit> hits;
        for (int k = 0; k < g_nObjs; ++k) {
            const Obj& o = g_objs[k];
            if (o.n < kFrames * 0.5) continue;
            const double n = o.n;
            const double mT = (t == 0 ? o.sY : o.sP) / n;
            const double vT = (t == 0 ? o.sYY : o.sPP) / n - mT * mT;
            if (vT < 1e-4) continue;
            for (uint32_t i = 0; i < kFloats; ++i) {
                const Acc& a = o.acc[i];
                if (a.n < n * 0.95) continue;
                const double m = a.s / a.n, v = a.ss / a.n - m * m;
                if (v < 1e-10) continue;
                const double cov = (t == 0 ? a.sy : a.sp) / a.n - m * mT;
                const double r = cov / std::sqrt(v * vT);
                if (std::fabs(r) < 0.97) continue;
                hits.push_back({ r, cov / vT, k, i * 4, o.last[i] });
            }
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return std::fabs(a.r) > std::fabs(b.r); });
        std::string out = std::string("CAMSTATE: floats tracking the game camera's ") + (t == 0 ? "YAW" : "PITCH") + " (|r| >= 0.97, "
            + std::to_string(hits.size()) + " found):";
        for (size_t h = 0; h < hits.size() && h < 40; ++h) {
            char b[200];
            const Hit& x = hits[h];
            snprintf(b, sizeof(b), "\n  %s @0x%llx +0x%x: r %.4f, slope %.3f, now %.4f", g_objs[x.obj].cls.c_str(),
                     static_cast<unsigned long long>(g_objs[x.obj].ptr), x.off, x.r, x.slope, x.last);
            out += b;
        }
        modlog::Line(out);
    }
    for (int t = 0; t < 2; ++t) {
        std::vector<Hit> hits;
        for (int k = 0; k < g_nObjs; ++k) {
            const Obj& o = g_objs[k];
            if (o.dn < 200) continue;
            const double mT = (t == 0 ? o.dsY : o.dsP) / o.dn;
            const double vT = (t == 0 ? o.dsYY : o.dsPP) / o.dn - mT * mT;
            if (vT < 1e-10) continue;
            for (uint32_t i = 0; i < kFloats; ++i) {
                const Acc& a = o.acc[i];
                if (a.dn < o.dn * 0.9) continue;
                const double m = a.ds / a.dn, v = a.dss / a.dn - m * m;
                if (v < 1e-14) continue;
                const double cov = (t == 0 ? a.dsy : a.dsp) / a.dn - m * mT;
                const double r = cov / std::sqrt(v * vT);
                if (std::fabs(r) < 0.9) continue;
                hits.push_back({ r, cov / vT, k, i * 4, o.last[i] });
            }
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return std::fabs(a.r) > std::fabs(b.r); });
        std::string out = std::string("CAMSTATE: floats whose frame-to-frame CHANGES track the game camera's ") + (t == 0 ? "YAW" : "PITCH")
            + " changes (|r| >= 0.9, wraps ignored, " + std::to_string(hits.size()) + " found):";
        for (size_t h = 0; h < hits.size() && h < 40; ++h) {
            char b[200];
            const Hit& x = hits[h];
            snprintf(b, sizeof(b), "\n  %s @0x%llx +0x%x: r %.4f, slope %.3f, now %.4f", g_objs[x.obj].cls.c_str(),
                     static_cast<unsigned long long>(g_objs[x.obj].ptr), x.off, x.r, x.slope, x.last);
            out += b;
        }
        modlog::Line(out);
    }
    std::string objs = "CAMSTATE: objects sampled:";
    for (int k = 0; k < g_nObjs; ++k) {
        char b[160];
        snprintf(b, sizeof(b), " %s @0x%llx (%d frames)", g_objs[k].cls.c_str(), static_cast<unsigned long long>(g_objs[k].ptr),
                 static_cast<int>(g_objs[k].n));
        objs += b;
    }
    modlog::Line(objs);
}

}  // namespace

void CamStateProbeGameThread(uintptr_t component, const float q[4]) {
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float fx = 2.0f * (x * y - w * z), fy = 1.0f - 2.0f * (x * x + z * z), fz = 2.0f * (y * z + w * x);
        const double yaw = std::atan2(-fx, fy) * 57.29578;
        const double pitch = std::asin(fz < -1.0f ? -1.0f : (fz > 1.0f ? 1.0f : fz)) * 57.29578;
        uintptr_t ctrl = 0;
        const bool haveCtrl = component && Rd(component + 0xD20 + 0x20, &ctrl) && ctrl && ClassName(ctrl) == "ExplorationCameraController";
        if (const int k = g_kick.exchange(0)) {
            if (!haveCtrl) {
                modlog::Line("CAMWRITE: the base camera controller isn't the ExplorationCameraController right now.");
            } else {
                float* v = reinterpret_cast<float*>(ctrl);
                g_kickVals0[0] = v[0x12D4 / 4]; g_kickVals0[1] = v[0x12D8 / 4]; g_kickVals0[2] = v[0x12EC / 4]; g_kickVals0[3] = v[0x12F0 / 4];
                if (k == 1) { v[0x12D4 / 4] += 0.3490659f; v[0x12D8 / 4] += 0.3490659f; }
                else { v[0x12EC / 4] -= 0.1745329f; v[0x12F0 / 4] -= 0.1745329f; }
                g_kickYaw0 = yaw;
                g_kickPitch0 = pitch;
                g_kickWatch = 0;
                char b[200];
                snprintf(b, sizeof(b), "CAMWRITE: %s -- game camera yaw %.2f pitch %.2f deg; state yaw %.4f/%.4f pitch %.4f/%.4f", k == 1
                         ? "yaw +20 deg written" : "pitch 10 deg up written", yaw, pitch, g_kickVals0[0], g_kickVals0[1], g_kickVals0[2], g_kickVals0[3]);
                modlog::Line(b);
            }
        } else if (g_kickWatch >= 0) {
            ++g_kickWatch;
            if (g_kickWatch == 36 || g_kickWatch == 144 || g_kickWatch == 288) {
                float s[4] = {};
                if (haveCtrl) { const float* v = reinterpret_cast<const float*>(ctrl); s[0] = v[0x12D4 / 4]; s[1] = v[0x12D8 / 4]; s[2] = v[0x12EC / 4]; s[3] = v[0x12F0 / 4]; }
                char b[220];
                snprintf(b, sizeof(b), "CAMWRITE: %.2f s later -- game camera yaw %+.2f pitch %+.2f deg from before; state yaw %.4f/%.4f pitch %.4f/%.4f",
                         g_kickWatch / 144.0, yaw - g_kickYaw0, pitch - g_kickPitch0, s[0], s[1], s[2], s[3]);
                modlog::Line(b);
                if (g_kickWatch == 288) g_kickWatch = -1;
            }
        }
    }
    int st = g_state.load(std::memory_order_relaxed);
    if (st == 1) {
        for (int i = 0; i < kMaxObjs; ++i) g_objs[i] = Obj{};
        g_nObjs = 0;
        g_frames = 0;
        g_haveYaw = false;
        g_yawUnwrapped = 0.0;
        nLara = -1;
        g_state.store(st = 2);
    }
    if (st != 2 || !component) return;
    if (g_aimMode) {
        float w2 = 0.0f;
        if (GetTickCount64() - g_startMs > 120000) { g_state.store(3); return; }
        if (!Rd(component + 0xC70, &w2) || w2 <= 0.5f) return;
    }
    // Forward = local +Y; world Z up.
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = 2.0f * (x * y - w * z), fy = 1.0f - 2.0f * (x * x + z * z), fz = 2.0f * (y * z + w * x);
    const double yaw = std::atan2(-fx, fy);
    const double pitch = std::asin(fz < -1.0f ? -1.0f : (fz > 1.0f ? 1.0f : fz));
    if (!g_haveYaw) { g_haveYaw = true; g_lastYaw = yaw; }
    double dy = yaw - g_lastYaw;
    while (dy > 3.14159265) dy -= 6.2831853;
    while (dy < -3.14159265) dy += 6.2831853;
    g_yawUnwrapped += dy;
    g_lastYaw = yaw;
    Sample(component, g_yawUnwrapped, pitch);
    static const uint16_t kLists[7][2] = { { 0xF00, 0xF08 }, { 0xF18, 0xF20 }, { 0xF30, 0xF38 }, { 0xF48, 0xF50 },
                                           { 0xF60, 0xF68 }, { 0xF78, 0xF80 }, { 0xF90, 0xF98 } };
    uintptr_t ctrl = 0;
    if (Rd(component + 0xD20 + 0x20, &ctrl) && ctrl) Sample(ctrl, g_yawUnwrapped, pitch);
    for (const auto& l : kLists) {
        uint32_t count = 0;
        uintptr_t arr = 0;
        if (!Rd(component + l[0], &count) || !Rd(component + l[1], &arr) || !arr) continue;
        for (uint32_t i = 0; i < count && i < 8; ++i) {
            if (Rd(arr + i * 0x1E0 + 0x20, &ctrl) && ctrl) Sample(ctrl, g_yawUnwrapped, pitch);
        }
    }
    // Lara's own components whose class says player / aim / camera / locomotion / character (the aim camera may sit
    // behind her with the stick turning her body).
    {
        if (nLara < 0) {
            nLara = 0;
            uintptr_t dir = 0, buckets = 0, node = 0;
            const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            uint64_t h = 0;
            if (Rd(exe + 0x16BB1F0, &dir) && dir && Rd(dir + 0x68, &buckets) && buckets && Rd(buckets, &node)) {
                for (int guard = 0; node && guard < 64; ++guard) {
                    uint32_t key = ~0u;
                    if (!Rd(node + 0x10, &key)) break;
                    if (key == 0) { uintptr_t obj = 0; if (Rd(node + 0x520, &obj) && obj) Rd(obj + 0x20, &h); break; }
                    if (!Rd(node, &node)) break;
                }
            }
            const uintptr_t e = (h >> 32) && static_cast<uint32_t>(h) < 20000 ? exe + 0x301D0C0 + static_cast<uintptr_t>(static_cast<uint32_t>(h)) * 0x120 : 0;
            uint64_t check = 0;
            uint32_t count = 0;
            uintptr_t comps = 0;
            if (e && Rd(e + 0x110, &check) && check == h && Rd(e + 0xD0, &count) && Rd(e + 0xD8, &comps) && comps) {
                std::string names;
                for (uint32_t i = 0; i < count && i < 512 && nLara < 16; ++i) {
                    uintptr_t c = 0;
                    if (!Rd(comps + i * 8, &c) || !c) continue;
                    const std::string n = ClassName(c);
                    if (n.find("Player") != std::string::npos || n.find("Aim") != std::string::npos || n.find("Camera") != std::string::npos
                        || n.find("Locomotion") != std::string::npos || n.find("Character") != std::string::npos || n.find("Transform") != std::string::npos) {
                        laraComps[nLara++] = c;
                        names += " " + n;
                    }
                }
                modlog::Line("CAMSTATE: also sampling Lara's components:" + names);
            }
        }
        for (int i = 0; i < nLara; ++i) Sample(laraComps[i], g_yawUnwrapped, pitch);
    }
    if (++g_frames >= kFrames) g_state.store(3);
}

void CamStateProbeOnPresent() {
    static bool cWas = false;
    const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool c = (GetAsyncKeyState('C') & 0x8000) != 0;
    if (c && !cWas && ca && g_state.load() == 0) {
        g_aimMode = CameraAimingNow();
        g_startMs = GetTickCount64();
        g_state.store(1);
        modlog::Line(g_aimMode ? "CAMSTATE (aim mode): sampling only while the bow is drawn (aim camera weight > 0.5), until 12.5 s of it or 120 s -- "
                                 "draw the bow and turn with the RIGHT STICK, repeat."
                               : "CAMSTATE: sampling the camera controllers for ~12 s -- turn the camera with the RIGHT STICK: left/right, then up/down.");
        PlayCue(Cue::On);
    }
    cWas = c;
    static bool eWas = false, iWas = false;
    const bool e = (GetAsyncKeyState('E') & 0x8000) != 0, i = (GetAsyncKeyState('I') & 0x8000) != 0;
    (void)e;   // Ctrl+Alt+E is head drive's toggle now (head_drive.cpp)
    (void)i;   // Ctrl+Alt+I is AFW's toggle now (afw.cpp)
    eWas = e;
    iWas = i;
    if (g_state.load() == 3) {
        Report();
        g_state.store(0);
        PlayCue(Cue::Success);
    }
}

}  // namespace vrmod
