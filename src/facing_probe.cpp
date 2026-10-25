#include "facing_probe.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio_cue.h"
#include "aim_watch.h"
#include "face_track.h"
#include "logger.h"

namespace vrmod {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTestRad = 15.0f * kPi / 180.0f;   // the test turn
constexpr int kWatchPresents = 6, kSettlePresents = 8;
constexpr size_t kMaxHits = 4096, kMaxCands = 96;

float Wrap(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
float YawOf(float x, float y) { return std::atan2(-x, y); }   // world yaw as the camera's (Z up, Y forward)

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

// Hits: x within tol at p, then y, z either packed (p+4, p+8) or one row apart (p+16, p+32: translation in a 3x4's last
// column). kind 0 packed, 1 column.
int ScanRegionRaw(const uint8_t* base, size_t size, float x, float y, float z, float tol, uint64_t* out, uint8_t* kind, int room) {
    int n = 0;
    __try {
        const float* f = reinterpret_cast<const float*>(base);
        const size_t count = size / 4;
        for (size_t i = 0; i + 8 < count && n < room; ++i) {
            if (std::fabs(f[i] - x) >= tol) continue;
            if (std::fabs(f[i + 1] - y) < tol && std::fabs(f[i + 2] - z) < tol) { out[n] = reinterpret_cast<uint64_t>(f + i); kind[n++] = 0; }
            else if (std::fabs(f[i + 4] - y) < tol && std::fabs(f[i + 8] - z) < tol) { out[n] = reinterpret_cast<uint64_t>(f + i); kind[n++] = 1; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

enum CandType { MatRows, MatCols, Quat, QuatConj, ScalarRad, ScalarRadNeg, ScalarDeg, ScalarDegNeg };
const char* const kTypeName[] = { "matrix (rows = axes)", "matrix (columns = axes)", "quaternion", "quaternion (conjugate)",
                                  "yaw, radians", "yaw, radians (negated)", "yaw, degrees", "yaw, degrees (negated)" };

struct Cand {
    uint64_t addr;       // first byte the test changes
    size_t size;         // bytes it changes
    int type;
    uint64_t posAddr;    // the position hit it was found from
    std::string owner;   // RTTI class of the object holding it, + offset
    // test results
    float maxDelta = 0, endDelta = 0;
    int persisted = 0;   // Presents the written value was still there
    bool tested = false, writeFailed = false;
};

std::atomic<int> g_phase{ 0 };   // 0 idle, 1 scanning, 2 testing
std::vector<Cand> g_cands;       // scan thread fills it before phase 2; Present thread afterwards
float g_refPos[3], g_refAx[2];
std::string g_scanNote;
uint64_t g_imageLo = 0, g_imageHi = 0;
void* g_self = nullptr;
bool g_scalarsOk = false;   // her facing is clearly off the world axes (plain yaw values are tested)   // this DLL's allocation base

bool Unit(const float* v) {
    const float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    return std::isfinite(l) && std::fabs(l - 1.0f) < 0.03f;
}

// Local X and Y axes in world from a 3x3 (row stride in floats); rows=true: rows are the axes, else columns.
void Axes(const float* m, int stride, bool rows, float ax[3], float ay[3]) {
    for (int k = 0; k < 3; ++k) {
        ax[k] = rows ? m[k] : m[k * stride];
        ay[k] = rows ? m[stride + k] : m[k * stride + 1];
    }
}

bool MatchesRef(const float ax[3], const float ay[3]) {
    return Unit(ax) && Unit(ay) && std::fabs(Wrap(YawOf(ax[0], ax[1]) - g_refAx[0])) < 0.09f
           && std::fabs(Wrap(YawOf(ay[0], ay[1]) - g_refAx[1])) < 0.09f && std::fabs(ax[2]) < 0.5f && std::fabs(ay[2]) < 0.5f;
}

void QRot(const float q[4], const float v[3], float out[3]) {   // q v q*, q = (x, y, z, w)
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float cx = y * v[2] - z * v[1], cy = z * v[0] - x * v[2], cz = x * v[1] - y * v[0];
    out[0] = v[0] + 2.0f * (w * cx + y * cz - z * cy);
    out[1] = v[1] + 2.0f * (w * cy + z * cx - x * cz);
    out[2] = v[2] + 2.0f * (w * cz + x * cy - y * cx);
}

std::string RttiName(uint64_t vt) {
    if (vt < g_imageLo || vt >= g_imageHi) return "";
    uint64_t col = 0;
    int32_t sig = -1, td = 0;
    if (!SafeCopy(&col, vt - 8, 8) || col < g_imageLo || col >= g_imageHi || !SafeCopy(&sig, col, 4) || sig != 1 || !SafeCopy(&td, col + 0x0C, 4))
        return "";
    char name[64] = {};
    if (!SafeCopy(name, g_imageLo + static_cast<uint32_t>(td) + 0x10, 63)) return "";
    name[63] = 0;
    if (strncmp(name, ".?AV", 4) != 0 && strncmp(name, ".?AU", 4) != 0) return "";
    return std::string(name + 4, strnlen(name + 4, 55));
}

std::string OwnerOf(uint64_t a) {
    for (uint64_t back = 0; back <= 0x1000; back += 8) {
        uint64_t q = 0;
        if (!SafeCopy(&q, (a & ~7ull) - back, 8)) break;
        const std::string n = RttiName(q);
        if (!n.empty()) {
            char b[96];
            snprintf(b, sizeof(b), "%s +0x%llX", n.c_str(), static_cast<unsigned long long>(a - ((a & ~7ull) - back)));
            return b;
        }
    }
    return "?";
}

// The object holding address a: its vtable's RTTI name and start (0 if none within 0x1000 bytes before it).
uint64_t OwnerBase(uint64_t a, std::string* name) {
    for (uint64_t back = 0; back <= 0x1000; back += 8) {
        uint64_t q = 0;
        if (!SafeCopy(&q, (a & ~7ull) - back, 8)) break;
        const std::string n = RttiName(q);
        if (!n.empty()) {
            if (name) *name = n;
            return (a & ~7ull) - back;
        }
    }
    return 0;
}

// Round 2 (2026-10-11): the test showed the render matrices (SceneEntity) are rebuilt every frame from the gameplay
// object, Player, whose yaw floats (+0x3C8, +0x468) are recomputed too. So record the Player object every Present for
// 8 s while she turns, with her model's facing, and find the fields that follow it -- whatever their form.
constexpr bool kRecordMode = true;
constexpr bool kPlayerTestMode = false;  // round 3: test the fields round 2 found (instead of recording)
// Round 4: round 3 showed those fields are copies, rewritten every frame (writing them never turned her). Watch which
// instructions write them (aim_watch.cpp, hardware write watchpoints) -- their code reads her real orientation.
constexpr bool kWatchWritersMode = false;
constexpr size_t kRecBytes = 0x2000;
constexpr double kRecSeconds = 8.0;
uint64_t g_recBase = 0, g_refPosHit = 0;
uintptr_t g_watchAddrs[4] = {};
bool g_instanceMode = true;   // round 5 (see kInstanceTestMode)
const char* const* g_watchLabels = nullptr;
struct RecSample { double t; float pos[3]; float ax[2]; uint32_t fresh; uint8_t bytes[kRecBytes]; };
std::vector<RecSample> g_rec;

void AddCand(uint64_t addr, size_t size, int type, uint64_t pos) {
    for (const Cand& c : g_cands) if (c.addr == addr && c.type == type) return;
    if (g_cands.size() >= kMaxCands) return;
    Cand c{};
    c.addr = addr;
    c.size = size;
    c.type = type;
    c.posAddr = pos;
    c.owner = OwnerOf(addr);
    g_cands.push_back(c);
}

// Rotations beside one position hit.
void Examine(uint64_t p, uint8_t kind) {
    float m[16];
    if (kind == 0) {
        // Row-vector 4x4 with the translation as row 3 (as the GPU's World): rows at p-48, p-32, p-16.
        if (SafeCopy(m, p - 48, 48)) {
            float ax[3], ay[3];
            Axes(m, 4, true, ax, ay);
            if (MatchesRef(ax, ay)) AddCand(p - 48, 44, MatRows, p);
            Axes(m, 4, false, ax, ay);
            if (MatchesRef(ax, ay)) AddCand(p - 48, 44, MatCols, p);
        }
        // Position + quaternion: the quaternion right after (packed or aligned) or right before.
        const int64_t qoff[] = { 12, 16, -16 };
        for (int64_t o : qoff) {
            float q[4];
            if (!SafeCopy(q, p + o, 16)) continue;
            const float l = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
            if (!std::isfinite(l) || std::fabs(l - 1.0f) > 0.02f) continue;
            const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 };
            float ax[3], ay[3];
            QRot(q, X, ax);
            QRot(q, Y, ay);
            if (MatchesRef(ax, ay)) AddCand(p + o, 16, Quat, p);
            const float qc[4] = { -q[0], -q[1], -q[2], q[3] };
            QRot(qc, X, ax);
            QRot(qc, Y, ay);
            if (MatchesRef(ax, ay)) AddCand(p + o, 16, QuatConj, p);
        }
    } else {
        // 3x4 with the translation as the last column: rows (r0 r1 r2 t) at p-12, p+4, p+20.
        if (SafeCopy(m, p - 12, 48)) {
            float ax[3], ay[3];
            Axes(m, 4, true, ax, ay);
            if (MatchesRef(ax, ay)) AddCand(p - 12, 44, MatRows, p);
            Axes(m, 4, false, ax, ay);
            if (MatchesRef(ax, ay)) AddCand(p - 12, 44, MatCols, p);
        }
    }
    // Plain yaw floats near the position (within 0x200 bytes): her facing / model axes, radians or degrees, either sign.
    // Only when she faces well off the world axes (2026-10-11: facing 179.7 deg, the references were ~0, pi/2 and pi, and
    // writing into one of the many small floats / constants near those crashed the game).
    if (!g_scalarsOk) return;
    float w[256];
    if (!SafeCopy(w, p - 0x200, sizeof w)) return;
    const float refs[4] = { g_refAx[0], g_refAx[1], Wrap(g_refAx[1] + kPi), Wrap(g_refAx[0] + kPi) };
    for (int i = 0; i < 256; ++i) {
        const float v = w[i];
        if (!std::isfinite(v) || std::fabs(v) > 400.0f || v == 0.0f) continue;
        for (float r : refs) {
            const uint64_t a = p - 0x200 + i * 4;
            if (std::fabs(v) <= 7.0f && std::fabs(Wrap(v - r)) < 0.004f) AddCand(a, 4, ScalarRad, p);
            else if (std::fabs(v) <= 7.0f && std::fabs(Wrap(-v - r)) < 0.004f) AddCand(a, 4, ScalarRadNeg, p);
            else if (std::fabs(Wrap(v * kPi / 180.0f - r)) < 0.004f) AddCand(a, 4, ScalarDeg, p);
            else if (std::fabs(Wrap(-v * kPi / 180.0f - r)) < 0.004f) AddCand(a, 4, ScalarDegNeg, p);
        }
    }
}

void ScanThread() {
    const double t0 = GetTickCount64();
    std::vector<uint64_t> hits(kMaxHits);
    std::vector<uint8_t> kinds(kMaxHits);
    size_t nh = 0;
    uint64_t scannedMb = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* p = nullptr;
    while (reinterpret_cast<uint64_t>(p) < 0x7FFFFFFF0000ull && VirtualQuery(p, &mbi, sizeof mbi) && nh < kMaxHits) {
        const DWORD prot = mbi.Protect;
        const bool rw = mbi.State == MEM_COMMIT && (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(prot & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE));
        // Not the mod's own memory (its copies of her pose), not the GPU buffers the game maps (next frame's draw data).
        if (rw && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) && mbi.AllocationBase != g_self
            && !FaceTrackIsMappedGpuMemory(reinterpret_cast<uint64_t>(mbi.BaseAddress), mbi.RegionSize)) {
            const int n = ScanRegionRaw(static_cast<const uint8_t*>(mbi.BaseAddress), mbi.RegionSize, g_refPos[0], g_refPos[1], g_refPos[2], 2.0f,
                                        hits.data() + nh, kinds.data() + nh, static_cast<int>(kMaxHits - nh));
            nh += n;
            scannedMb += mbi.RegionSize >> 20;
        }
        p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    if (kRecordMode) {
        g_recBase = 0;
        for (size_t i = 0; i < nh && !g_recBase; ++i) {
            std::string n;
            const uint64_t b0 = OwnerBase(hits[i], &n);
            if (b0 && n == "Player@@") { g_recBase = b0; g_refPosHit = hits[i]; }
        }
        char lb[220];
        if (g_recBase) {
            if (g_instanceMode) {
                snprintf(lb, sizeof(lb), "FACINGPROBE: Player object at 0x%llX -- testing her instance heading: stand still",
                         static_cast<unsigned long long>(g_recBase));
                modlog::Line(lb);
                g_phase.store(6);
                PlayCue(Cue::On);
                return;
            }
            if (kWatchWritersMode) {
                const uintptr_t a[4] = { static_cast<uintptr_t>(g_recBase + 0x1B20), static_cast<uintptr_t>(g_recBase + 0x3C8),
                                         static_cast<uintptr_t>(g_recBase + 0x468), static_cast<uintptr_t>(g_refPosHit) };
                static const char* const l[4] = { "Player +0x1B20 facing direction x", "Player +0x3C8 heading angle", "Player +0x468 heading angle",
                                                  "Player +0x550 position x" };
                memcpy(g_watchAddrs, a, sizeof a);
                g_watchLabels = l;
                snprintf(lb, sizeof(lb), "FACINGPROBE: Player object at 0x%llX -- watching who writes her facing copies and position (1.5 s)",
                         static_cast<unsigned long long>(g_recBase));
                modlog::Line(lb);
                g_phase.store(5);
                return;
            }
            if (kPlayerTestMode) {
                snprintf(lb, sizeof(lb), "FACINGPROBE: Player object at 0x%llX (her position at +0x%llX) -- testing her facing fields: stand still",
                         static_cast<unsigned long long>(g_recBase), static_cast<unsigned long long>(g_refPosHit - g_recBase));
                modlog::Line(lb);
                g_phase.store(4);
                PlayCue(Cue::On);
                return;
            }
            snprintf(lb, sizeof(lb), "FACINGPROBE: Player object at 0x%llX (her position at +0x%llX) -- recording it for %.0f s: walk her round in a slow circle now",
                     static_cast<unsigned long long>(g_recBase), static_cast<unsigned long long>(g_refPosHit - g_recBase), kRecSeconds);
            modlog::Line(lb);
            g_rec.clear();
            g_rec.reserve(800);
            g_phase.store(3);
            PlayCue(Cue::On);
        } else {
            snprintf(lb, sizeof(lb), "FACINGPROBE: %zu places hold her position, none inside a Player object -- nothing recorded", nh);
            modlog::Line(lb);
            g_phase.store(0);
            PlayCue(Cue::Failure);
        }
        return;
    }
    {
        const float a = std::fabs(std::remainder(g_refAx[1], kPi * 0.5f));   // distance from the nearest multiple of 90 deg
        g_scalarsOk = a > 0.2f;
    }
    for (size_t i = 0; i < nh; ++i) Examine(hits[i], kinds[i]);
    // Rotations first, plain yaw values last; the list goes to a file before any test (a crash then names the culprit).
    std::stable_sort(g_cands.begin(), g_cands.end(), [](const Cand& a, const Cand& b) { return (a.type >= ScalarRad) < (b.type >= ScalarRad); });
    {
        std::ostringstream o;
        for (size_t i = 0; i < g_cands.size(); ++i) {
            char b[256];
            snprintf(b, sizeof(b), "%2zu 0x%llX %s [pos at 0x%llX] owner %s\n", i + 1, static_cast<unsigned long long>(g_cands[i].addr),
                     kTypeName[g_cands[i].type], static_cast<unsigned long long>(g_cands[i].posAddr), g_cands[i].owner.c_str());
            o << b;
        }
        std::ofstream("vrmod_facingprobe_candidates.txt", std::ios::binary) << o.str();
    }
    if (!g_scalarsOk) modlog::Line("FACINGPROBE: she faces close to a world axis -- plain yaw values skipped (turn her to a diagonal to include them)");
    char b[256];
    snprintf(b, sizeof(b), "FACINGPROBE: scanned %llu MB in %.1f s: %zu places hold her position, %zu rotations / yaw values beside them -- testing each (15 deg turn)",
             static_cast<unsigned long long>(scannedMb), (GetTickCount64() - t0) / 1000.0, nh, g_cands.size());
    modlog::Line(b);
    g_phase.store(g_cands.empty() ? 0 : 2);
    if (g_cands.empty()) PlayCue(Cue::Failure);
}

// The changed bytes for a test turn of +kTestRad (world yaw).
bool Turned(const Cand& c, const uint8_t* orig, uint8_t* out) {
    memcpy(out, orig, c.size);
    const float cs = std::cos(kTestRad), sn = std::sin(kTestRad);
    auto rot = [&](float* v) { const float x = v[0], y = v[1]; v[0] = x * cs - y * sn; v[1] = x * sn + y * cs; };
    if (c.type == MatRows || c.type == MatCols) {
        float m[11];
        memcpy(m, out, 44);
        for (int k = 0; k < 3; ++k) {
            if (c.type == MatRows) {
                rot(m + 4 * k);
            } else {
                float v[3] = { m[k], m[4 + k], m[8 + k] };
                rot(v);
                m[k] = v[0]; m[4 + k] = v[1]; m[8 + k] = v[2];
            }
        }
        memcpy(out, m, 44);
        return true;
    }
    if (c.type == Quat || c.type == QuatConj) {
        float q[4];
        memcpy(q, out, 16);
        const float hz = std::sin(kTestRad * 0.5f), hw = std::cos(kTestRad * 0.5f);
        // world turn r = (0,0,hz,hw): Quat -> r*q; QuatConj (stores the inverse) -> q*conj(r)
        const float a[4] = { 0, 0, hz, hw };
        const float* L = c.type == Quat ? a : q;
        const float rc[4] = { 0, 0, -hz, hw };
        const float* R = c.type == Quat ? q : rc;
        const float o[4] = { L[3] * R[0] + L[0] * R[3] + L[1] * R[2] - L[2] * R[1], L[3] * R[1] - L[0] * R[2] + L[1] * R[3] + L[2] * R[0],
                             L[3] * R[2] + L[0] * R[1] - L[1] * R[0] + L[2] * R[3], L[3] * R[3] - L[0] * R[0] - L[1] * R[1] - L[2] * R[2] };
        memcpy(out, o, 16);
        return true;
    }
    float v;
    memcpy(&v, out, 4);
    const float d = (c.type == ScalarDeg || c.type == ScalarDegNeg) ? 15.0f : kTestRad;
    v += (c.type == ScalarRadNeg || c.type == ScalarDegNeg) ? -d : d;
    memcpy(out, &v, 4);
    return true;
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

void Report() {
    std::ostringstream o;
    o << "Facing probe " << TimeStamp() << ": her model root at (" << g_refPos[0] << ", " << g_refPos[1] << ", " << g_refPos[2]
      << "), model X axis yaw " << g_refAx[0] * 57.29578f << " deg, Y axis yaw " << g_refAx[1] * 57.29578f << " deg\n"
      << "Test: each value turned by +15 deg (world yaw); model = how far her drawn model turned (max / after 6 Presents);\n"
      << "kept = Presents the written value stayed before the game replaced it (6 = it stayed).\n\n";
    int drives = 0;
    for (const Cand& c : g_cands) {
        char b[400];
        const bool moved = std::fabs(c.maxDelta) > 8.0f;
        if (moved) ++drives;
        snprintf(b, sizeof(b), "%s 0x%llX  %-26s  model %+6.1f / %+6.1f deg  kept %d/6  %s  [pos at 0x%llX]  owner %s\n",
                 c.writeFailed ? "WRITE FAILED" : moved ? (c.persisted >= kWatchPresents ? "DRIVES+STAYS" : "DRIVES,REPLACED") : (c.persisted >= kWatchPresents ? "copy (stays)" : "copy (replaced)"),
                 static_cast<unsigned long long>(c.addr), kTypeName[c.type], c.maxDelta, c.endDelta, c.persisted,
                 "", static_cast<unsigned long long>(c.posAddr), c.owner.c_str());
        o << b;
    }
    const std::string name = "vrmod_facingprobe_" + TimeStamp() + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();
    char b[200];
    snprintf(b, sizeof(b), "FACINGPROBE: %zu values tested, %d turned her model -- details in %s", g_cands.size(), drives, name.c_str());
    modlog::Line(b);
    for (const Cand& c : g_cands) {
        if (std::fabs(c.maxDelta) <= 8.0f) continue;
        snprintf(b, sizeof(b), "FACINGPROBE:   0x%llX %s: model %+.1f deg, kept %d/6, owner %s", static_cast<unsigned long long>(c.addr),
                 kTypeName[c.type], c.maxDelta, c.persisted, c.owner.c_str());
        modlog::Line(b);
    }
    PlayCue(drives ? Cue::Success : Cue::Failure);
}

// Present-thread test of one candidate after another.
void TestTick() {
    static size_t idx = 0;
    static int step = -1, settle = 0;
    static uint8_t orig[48], wrote[48];
    static float yaw0 = 0;
    if (settle > 0) { --settle; return; }
    if (idx >= g_cands.size()) {
        Report();
        idx = 0;
        step = -1;
        g_phase.store(0);
        return;
    }
    Cand& c = g_cands[idx];
    float pos[3], ax[2];
    if (!FaceRootPose(pos, ax, 250.0)) return;   // wait for a fresh reading
    if (step < 0) {
        char lb[200];
        snprintf(lb, sizeof(lb), "FACINGPROBE: test %zu/%zu 0x%llX %s, owner %s", idx + 1, g_cands.size(), static_cast<unsigned long long>(c.addr),
                 kTypeName[c.type], c.owner.c_str());
        modlog::Line(lb);
        if (!SafeCopy(orig, c.addr, c.size) || !Turned(c, orig, wrote) || !SafeWrite(c.addr, wrote, c.size)) {
            c.writeFailed = c.tested = true;
            ++idx;
            return;
        }
        yaw0 = ax[1];
        step = 0;
        return;
    }
    uint8_t now[48];
    const bool same = SafeCopy(now, c.addr, c.size) && memcmp(now, wrote, c.size) == 0;
    if (same && c.persisted == step) ++c.persisted;
    const float d = Wrap(ax[1] - yaw0) * 57.29578f;
    if (std::fabs(d) > std::fabs(c.maxDelta)) c.maxDelta = d;
    c.endDelta = d;
    if (++step < kWatchPresents) return;
    if (same) SafeWrite(c.addr, orig, c.size);   // put it back (the game didn't replace it)
    c.tested = true;
    step = -1;
    ++idx;
    settle = kSettlePresents;
}

// Round 3 (2026-10-11): the recording found, in the Player object, a unit direction vector at +0x1B20/+0x1B24
// (sin(facing), -cos(facing)) and two heading angles at +0x3C8 / +0x468 (facing + ~180 deg, radians), all in step with
// her model. Test each by writing a 15 deg turn: once, and rewritten every Present for 1 s.
constexpr uint64_t kOffVec = 0x1B20, kOffAng1 = 0x3C8, kOffAng2 = 0x468;
struct PTest { const char* name; bool vec, ang; int hold; };
const PTest kPTests[] = { { "direction vector +0x1B20, once", true, false, 1 },
                          { "heading angles +0x3C8/+0x468, once", false, true, 1 },
                          { "vector + angles, once", true, true, 1 },
                          { "vector + angles, rewritten every Present for 1 s", true, true, 72 } };
constexpr int kPTestCount = sizeof(kPTests) / sizeof(kPTests[0]);
struct PResult { float maxDelta = 0, endDelta = 0; int kept = 0, frames = 0; bool readFailed = false; };
PResult g_presults[kPTestCount];

void PTestReport() {
    char b[260];
    for (int i = 0; i < kPTestCount; ++i) {
        const PResult& r = g_presults[i];
        snprintf(b, sizeof(b), "FACINGPROBE: test %d, %s: her model turned %+.1f deg at most, %+.1f deg at the end; our value kept in %d of %d Presents%s",
                 i + 1, kPTests[i].name, r.maxDelta, r.endDelta, r.kept, r.frames, r.readFailed ? " (READ FAILED)" : "");
        modlog::Line(b);
    }
    bool any = false;
    for (const PResult& r : g_presults) any = any || std::fabs(r.maxDelta) > 8.0f;
    PlayCue(any ? Cue::Success : Cue::Failure);
}

void PTestTick() {
    static int test = 0, step = -1, settle = 0;
    static float origVec[2], origAng[2], wVec[2], wAng[2], yaw0;
    if (settle > 0) { --settle; return; }
    if (test >= kPTestCount) {
        PTestReport();
        test = 0;
        step = -1;
        g_phase.store(0);
        return;
    }
    const PTest& T = kPTests[test];
    PResult& R = g_presults[test];
    float pos[3], ax[2];
    if (!FaceRootPose(pos, ax, 250.0)) return;
    const uint64_t b = g_recBase;
    if (step < 0) {
        R = PResult{};
        if (!SafeCopy(origVec, b + kOffVec, 8) || !SafeCopy(&origAng[0], b + kOffAng1, 4) || !SafeCopy(&origAng[1], b + kOffAng2, 4)) {
            R.readFailed = true;
            ++test;
            return;
        }
        const float cs = std::cos(kTestRad), sn = std::sin(kTestRad);
        // (sin f, -cos f) turned to f + 15 deg: the same rotation applied to the vector
        wVec[0] = origVec[0] * cs - origVec[1] * sn;
        wVec[1] = origVec[0] * sn + origVec[1] * cs;
        wAng[0] = Wrap(origAng[0] + kTestRad);
        wAng[1] = Wrap(origAng[1] + kTestRad);
        yaw0 = ax[1];
        char lb[160];
        snprintf(lb, sizeof(lb), "FACINGPROBE: test %d: %s", test + 1, T.name);
        modlog::Line(lb);
        step = 0;
    } else {
        // Did our value survive since the last write?
        float v[2], a[2];
        const bool okv = SafeCopy(v, b + kOffVec, 8), oka = SafeCopy(&a[0], b + kOffAng1, 4) && SafeCopy(&a[1], b + kOffAng2, 4);
        const bool keptV = !T.vec || (okv && std::fabs(v[0] - wVec[0]) < 1e-4f && std::fabs(v[1] - wVec[1]) < 1e-4f);
        const bool keptA = !T.ang || (oka && std::fabs(a[0] - wAng[0]) < 1e-4f && std::fabs(a[1] - wAng[1]) < 1e-4f);
        if (keptV && keptA) ++R.kept;
        ++R.frames;
        const float d = Wrap(ax[1] - yaw0) * 57.29578f;
        if (std::fabs(d) > std::fabs(R.maxDelta)) R.maxDelta = d;
        R.endDelta = d;
    }
    if (step < T.hold) {
        if (T.vec) SafeWrite(b + kOffVec, wVec, 8);
        if (T.ang) { SafeWrite(b + kOffAng1, &wAng[0], 4); SafeWrite(b + kOffAng2, &wAng[1], 4); }
    }
    if (++step < T.hold + kWatchPresents) return;
    // Put it back (her facing returns to where it was).
    if (T.vec) SafeWrite(b + kOffVec, origVec, 8);
    if (T.ang) { SafeWrite(b + kOffAng1, &origAng[0], 4); SafeWrite(b + kOffAng2, &origAng[1], 4); }
    step = -1;
    ++test;
    settle = 30;
}

// Round 5 (2026-10-11): the write watch + Ghidra showed the Player update (FUN_1409a77a0, given her instance) copies the
// instance's rotation (instance +0x30: angles about X, Y, Z in radians, Z = heading) into the Player each frame through
// FUN_1409ad290(player, angles) -- the Player's set-rotation, which also pushes it to *(player + 0x33E8) +0x30 with the
// "transform changed" flag 0x4000 at +0xC8. Test turning her by the instance heading (direct write) and by calling that
// function, on the game thread (FacingProbeGameThread), once and held for 1 s.
constexpr bool kInstanceTestMode = true;
constexpr uint32_t kKnownStamp = 0x64AC5C46;      // the SOTTR.exe build the addresses below are from
constexpr uint64_t kSetRotationRva = 0x9AD290;    // FUN_1409ad290
using SetRotation_t = void(__fastcall*)(uint64_t player, float* angles);
struct ITest { const char* name; bool call; int hold; };
const ITest kITests[] = { { "instance heading written once", false, 1 },
                          { "instance heading written every frame for 1 s", false, 72 },
                          { "set-rotation called once", true, 1 },
                          { "set-rotation called every frame for 1 s", true, 72 } };
constexpr int kITestCount = sizeof(kITests) / sizeof(kITests[0]);
PResult g_iresults[kITestCount];
uint64_t g_inst = 0;
SetRotation_t g_setRot = nullptr;
// Game-thread request: 1 write the heading (instance +0x38), 2 call set-rotation with it.
std::atomic<int> g_req{ 0 };
std::atomic<float> g_reqHeading{ 0.0f };
std::atomic<int> g_reqDone{ 0 };

bool CheckInstance() {
    char b[260];
    uint64_t inst = 0;
    float ipos[4], irot[4], prot[4];
    if (!SafeCopy(&inst, g_recBase + 0x33E8, 8) || !inst || !SafeCopy(ipos, inst + 0x10, 16) || !SafeCopy(irot, inst + 0x30, 16)
        || !SafeCopy(prot, g_recBase + 0x3C0, 16)) {
        modlog::Line("FACINGPROBE: Player +0x33E8 isn't a readable pointer -- no instance");
        return false;
    }
    const float dp = std::fabs(ipos[0] - g_refPos[0]) + std::fabs(ipos[1] - g_refPos[1]) + std::fabs(ipos[2] - g_refPos[2]);
    snprintf(b, sizeof(b), "FACINGPROBE: instance 0x%llX: position (%.1f, %.1f, %.1f) (%.0f cm from her model), angles (%.4f, %.4f, %.4f); Player +0x3C0 angles (%.4f, %.4f, %.4f)",
             static_cast<unsigned long long>(inst), ipos[0], ipos[1], ipos[2], dp, irot[0], irot[1], irot[2], prot[0], prot[1], prot[2]);
    modlog::Line(b);
    g_inst = inst;
    const HMODULE exe = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp == kKnownStamp) {
        g_setRot = reinterpret_cast<SetRotation_t>(reinterpret_cast<uint64_t>(exe) + kSetRotationRva);
    } else {
        modlog::Line("FACINGPROBE: a different SOTTR.exe build -- set-rotation tests skipped");
    }
    return std::fabs(irot[2] - prot[2]) < 0.01f;   // the instance's heading is what the Player holds
}

void ITestReport() {
    char b[260];
    bool any = false;
    for (int i = 0; i < kITestCount; ++i) {
        const PResult& r = g_iresults[i];
        any = any || std::fabs(r.maxDelta) > 8.0f;
        snprintf(b, sizeof(b), "FACINGPROBE: test %d, %s: her model turned %+.1f deg at most, %+.1f deg at the end; heading kept in %d of %d Presents%s",
                 i + 1, kITests[i].name, r.maxDelta, r.endDelta, r.kept, r.frames, r.readFailed ? " (skipped)" : "");
        modlog::Line(b);
    }
    PlayCue(any ? Cue::Success : Cue::Failure);
}

void ITestTick() {
    static int test = 0, step = -1, settle = 0;
    static float orig = 0, target = 0, yaw0 = 0;
    if (g_req.load() != 0) return;   // the game thread hasn't done the last request yet
    if (settle > 0) { --settle; return; }
    if (test >= kITestCount) {
        ITestReport();
        test = 0;
        step = -1;
        g_phase.store(0);
        return;
    }
    const ITest& T = kITests[test];
    PResult& R = g_iresults[test];
    float pos[3], ax[2];
    if (!FaceRootPose(pos, ax, 250.0)) return;
    if (step < 0) {
        R = PResult{};
        if (T.call && !g_setRot) { R.readFailed = true; ++test; return; }
        if (!SafeCopy(&orig, g_inst + 0x38, 4)) { R.readFailed = true; ++test; return; }
        target = Wrap(orig + kTestRad);
        yaw0 = ax[1];
        char lb[120];
        snprintf(lb, sizeof(lb), "FACINGPROBE: test %d: %s", test + 1, T.name);
        modlog::Line(lb);
        step = 0;
    } else {
        float h = 0;
        if (SafeCopy(&h, g_inst + 0x38, 4) && std::fabs(Wrap(h - target)) < 1e-3f) ++R.kept;
        ++R.frames;
        const float d = Wrap(ax[1] - yaw0) * 57.29578f;
        if (std::fabs(d) > std::fabs(R.maxDelta)) R.maxDelta = d;
        R.endDelta = d;
    }
    if (step < T.hold) {
        g_reqHeading.store(target);
        g_req.store(T.call ? 2 : 1);
        ++step;
        return;
    }
    if (++step < T.hold + 8) return;
    // Back to where she faced.
    g_reqHeading.store(orig);
    g_req.store(g_setRot ? 2 : 1);
    step = -1;
    ++test;
    settle = 30;
}

// Fields that follow her facing: for each float, the best of: an angle (radians / degrees, either sign, any constant
// offset) or a sine / cosine of it (full or half angle, as in a rotation matrix or a quaternion). Error over the samples.
void Analyse(const std::string& file) {
    std::vector<float> yaw;
    std::vector<size_t> idx;
    for (size_t i = 0; i < g_rec.size(); ++i) if (g_rec[i].fresh) { yaw.push_back(Wrap(g_rec[i].ax[1] + kPi)); idx.push_back(i); }
    float lo = 0, hi = 0, acc = 0;
    for (size_t i = 1; i < yaw.size(); ++i) { acc += Wrap(yaw[i] - yaw[i - 1]); lo = (std::min)(lo, acc); hi = (std::max)(hi, acc); }
    const float span = (hi - lo) * 57.29578f;
    struct Hit { size_t off; float err; const char* rel; };
    std::vector<Hit> best;
    auto at = [&](size_t j, size_t off) { float v; memcpy(&v, g_rec[idx[j]].bytes + off, 4); return v; };
    for (size_t off = 0; off + 4 <= kRecBytes; off += 4) {
        float vmin = 1e30f, vmax = -1e30f;
        bool finite = true;
        for (size_t j = 0; j < idx.size(); ++j) {
            const float v = at(j, off);
            if (!std::isfinite(v)) { finite = false; break; }
            vmin = (std::min)(vmin, v);
            vmax = (std::max)(vmax, v);
        }
        if (!finite || idx.size() < 20 || vmax - vmin < 1e-3f) continue;
        Hit h{ off, 1e9f, "" };
        // angle-like: the circular spread of k*v - yaw (radians or degrees, either sign)
        const struct { float scale; float sign; const char* name; } ang[] = { { 1, 1, "angle (rad)" }, { 1, -1, "angle (rad, negated)" },
                                                                              { kPi / 180, 1, "angle (deg)" }, { kPi / 180, -1, "angle (deg, negated)" } };
        for (const auto& a : ang) {
            float sx = 0, sy = 0;
            for (size_t j = 0; j < idx.size(); ++j) {
                const float d = Wrap(a.sign * at(j, off) * a.scale - yaw[j]);
                sx += std::cos(d);
                sy += std::sin(d);
            }
            const float c = std::atan2(sy, sx);
            float e = 0;
            for (size_t j = 0; j < idx.size(); ++j) {
                const float d = Wrap(a.sign * at(j, off) * a.scale - yaw[j] - c);
                e += d * d;
            }
            e = std::sqrt(e / idx.size()) * 57.29578f;   // degrees
            if (e < h.err) { h.err = e; h.rel = a.name; }
        }
        // trig-like: v = sin(k*yaw + c), k = 1 (matrix entry) or 1/2 (quaternion part, sign-free via |v|); c on a grid
        const struct { float k; const char* name; } trig[] = { { 1.0f, "sin/cos of it (matrix entry)" }, { 0.5f, "half-angle (quaternion part)" } };
        for (const auto& t : trig) {
            for (int g = 0; g < 72; ++g) {
                const float c = g * (2.0f * kPi / 72.0f);
                float e = 0;
                for (size_t j = 0; j < idx.size(); ++j) {
                    const float v = at(j, off), m = std::sin(t.k * yaw[j] + c);
                    const float d = t.k == 1.0f ? v - m : std::fabs(v) - std::fabs(m);
                    e += d * d;
                }
                e = std::sqrt(e / idx.size()) * 57.29578f;   // scaled like degrees, for ranking only
                if (e < h.err) { h.err = e; h.rel = t.name; }
            }
        }
        best.push_back(h);
    }
    std::sort(best.begin(), best.end(), [](const Hit& a, const Hit& b) { return a.err < b.err; });
    char b[240];
    snprintf(b, sizeof(b), "FACINGPROBE: recorded %zu Presents (%zu with her facing), she turned through %.0f deg -- raw data in %s",
             g_rec.size(), idx.size(), span, file.c_str());
    modlog::Line(b);
    for (size_t i = 0; i < best.size() && i < 12; ++i) {
        snprintf(b, sizeof(b), "FACINGPROBE:   Player +0x%03zX follows her facing as %s (error %.2f)", best[i].off, best[i].rel, best[i].err);
        modlog::Line(b);
    }
    if (span < 120.0f) modlog::Line("FACINGPROBE: she turned less than 120 deg -- the ranking is weak; record again turning her further");
}

void RecordTick() {
    static double t0 = 0;
    const double now = GetTickCount64() / 1000.0;
    if (g_rec.empty()) t0 = now;
    RecSample* r = &g_rec.emplace_back();
    r->t = now - t0;
    r->fresh = FaceRootPose(r->pos, r->ax, 50.0) ? 1 : 0;
    if (!SafeCopy(r->bytes, g_recBase, kRecBytes)) {
        g_rec.pop_back();
        modlog::Line("FACINGPROBE: the Player object can't be read any more -- recording stopped");
        g_phase.store(0);
        PlayCue(Cue::Failure);
        return;
    }
    if (now - t0 < kRecSeconds && g_rec.size() < 2000) return;
    const std::string name = "vrmod_facingrec_" + TimeStamp() + ".bin";
    {
        std::ofstream f(name, std::ios::binary);
        const char magic[8] = { 'F', 'A', 'C', 'E', 'R', 'E', 'C', '1' };
        const uint32_t n = static_cast<uint32_t>(g_rec.size()), size = static_cast<uint32_t>(kRecBytes);
        f.write(magic, 8);
        f.write(reinterpret_cast<const char*>(&n), 4);
        f.write(reinterpret_cast<const char*>(&size), 4);
        f.write(reinterpret_cast<const char*>(&g_recBase), 8);
        for (const RecSample& s : g_rec) f.write(reinterpret_cast<const char*>(&s), sizeof s);
    }
    // (Analyse(name) ran here and froze the game for a minute -- the raw file is analysed offline instead.)
    modlog::Line("FACINGPROBE: recording saved to " + name);
    g_rec.clear();
    g_rec.shrink_to_fit();
    g_phase.store(0);
    PlayCue(Cue::Success);
}

}  // namespace

void FacingProbeOnPresent() {
    static bool was = false;
    // Off since 2026-10-12: done (the body turn uses its result); Ctrl+Alt+F12 is the body census now (ui_probe.cpp).
    constexpr bool kHotkey = false;
    const bool down = kHotkey && (GetAsyncKeyState(VK_F12) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool pressed = down && !was;
    was = down;
    const int phase = g_phase.load();
    if (phase == 2) { TestTick(); return; }
    if (phase == 3) { RecordTick(); return; }
    if (phase == 4) { PTestTick(); return; }
    if (phase == 6) {   // round 5: check the instance link once, then the tests
        static bool checked = false;
        if (!checked) {
            checked = true;
            if (!CheckInstance()) {
                modlog::Line("FACINGPROBE: the instance doesn't hold her heading -- tests skipped");
                g_phase.store(0);
                checked = false;
                PlayCue(Cue::Failure);
                return;
            }
        }
        ITestTick();
        if (g_phase.load() == 0) checked = false;
        return;
    }
    if (phase == 5) {   // arm the watch from the Present thread (aim_watch finishes it 1.5 s later)
        g_phase.store(0);
        if (!AimWatchStart(g_watchAddrs, g_watchLabels, 4)) { modlog::Line("FACINGPROBE: the write watch didn't start"); PlayCue(Cue::Failure); }
        return;
    }
    if (!pressed || phase != 0) return;
    if (!FaceRootPose(g_refPos, g_refAx, 250.0)) {
        modlog::Line("FACINGPROBE: no fresh reading of her model (first person, Lara in view of the face draw) -- try again in gameplay");
        PlayCue(Cue::Failure);
        return;
    }
    if (!g_imageLo) {
        const HMODULE exe = GetModuleHandleW(nullptr);
        g_imageLo = reinterpret_cast<uint64_t>(exe);
        MEMORY_BASIC_INFORMATION mbi;
        uint64_t hi = g_imageLo;
        uint8_t* p = reinterpret_cast<uint8_t*>(exe);
        while (VirtualQuery(p, &mbi, sizeof mbi) && mbi.AllocationBase == exe) {
            hi = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
            p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }
        g_imageHi = hi;
    }
    if (!g_self) {
        HMODULE me = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&FacingProbeOnPresent), &me);
        g_self = me;
    }
    g_cands.clear();
    g_phase.store(1);
    char b[200];
    snprintf(b, sizeof(b), "FACINGPROBE: scanning for her position (%.1f, %.1f, %.1f), model Y axis yaw %.1f deg -- stand still",
             g_refPos[0], g_refPos[1], g_refPos[2], g_refAx[1] * 57.29578f);
    modlog::Line(b);
    PlayCue(Cue::On);
    std::thread(ScanThread).detach();
}

void FacingProbeGameThread() {
    const int r = g_req.load();
    if (r == 0 || !g_inst) return;
    const float h = g_reqHeading.load();
    if (r == 1) {
        SafeWrite(g_inst + 0x38, &h, 4);
    } else if (g_setRot) {
        alignas(16) float a[4];
        if (SafeCopy(a, g_inst + 0x30, 16)) {
            a[2] = h;
            g_setRot(g_recBase, a);
        }
    }
    g_req.store(0);
}

}  // namespace vrmod
