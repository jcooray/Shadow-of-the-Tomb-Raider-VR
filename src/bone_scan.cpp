#include "build_config.h"
#include "bone_scan.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio_cue.h"
#include "camera_interceptor.h"
#include "face_track.h"
#include "frame_log.h"
#include "logger.h"
#include "perf_stats.h"
#include "settings.h"

namespace vrmod {

namespace {

constexpr float kNearCm = 80.0f;          // a hit's translation must be this close to the in-head point
constexpr float kArrayCm = 400.0f;        // bones of a grown array must be within this of it
constexpr size_t kMaxHits = 200000;
constexpr size_t kMaxCandidates = 64;
constexpr int kMaxBones = 1024;
constexpr int kSamplePresents = 216;      // ~3 s at 72 Hz
constexpr double kSliceMs = 8.0;
constexpr size_t kChunk = 4u << 20;
constexpr uint32_t kOneBits = 0x3F800000u;

std::atomic<float> g_nearCm{ 0.0f };

float Tx, Ty, Tz;   // the in-head point (camera position at Ctrl+Z)
float Tq[4];        // camera rotation at Ctrl+Z (local X right, Y forward, Z up)

struct Region { uint8_t* base; size_t size; };
std::vector<Region> g_regions;
size_t g_cursor = 0, g_offset = 0;
uint64_t g_totalBytes = 0, g_scannedBytes = 0, g_skippedWcBytes = 0;
int g_faulted = 0;
double g_startMs = 0.0, g_lastProgressMs = 0.0;

std::vector<uint64_t> g_hits;

struct PtrHit { uint64_t at; bool viaDefault; uint16_t countField; };
struct Candidate {
    uint64_t base = 0;
    int count = 0;
    int seedIndex = 0;
    std::vector<PtrHit> ptrs;
    // sampling
    int samples = 0, changes = 0;
    std::vector<float> first, lo, hi;   // per bone xyz
    std::vector<float> last;
};
std::vector<Candidate> g_cands;
std::unordered_map<uint64_t, size_t> g_baseIndex;

enum Phase { Idle, Scan, Pointers, Sample };
Phase g_phase = Idle;
int g_sampleCount = 0;
double g_scanMs = 0.0, g_ptrMs = 0.0;
double g_sliceMs = kSliceMs;
constexpr float kNearRadiusAttach = 70.0f;
float g_nearRadiusCm = kNearCm;   // 80 cm for Ctrl+Z; the automatic locate uses kLocateCm
// The first in-game locate took ~25 s: the head was 54.6 cm from the offset camera when found, and earlier tries had it
// just outside the 80 cm scan / 60 cm plausibility radius (the offset camera moves with the run push and the orbit).
constexpr float kLocateCm = 200.0f;   // 150 missed for 4-5 s after loads (the camera still settling; located at 100-138 cm)
// The offset camera is only a rough hint: it hangs off the game's third-person boom, whose length changes with the camera
// mode. 2026-09-30 playthrough: heads found 71-185 cm from it, and one switch into first person right after a scripted
// camera found nothing within 200 cm four times running. After a failed locate the radius widens (LocateRadiusCm; the
// nearest double-buffered Lara-shaped skeleton still wins, so a wider search doesn't prefer someone further away).

// Bow / quiver finder (docs/FIRSTPERSON.md). Collapsing pose-buffer slots 94-110 didn't touch the bow: it is placed
// independently. Ctrl+Alt+B scans for 4x4 transforms within 70 cm of the head bone and BEHIND it (camera axes) that are
// not part of Lara's two pose buffers -- candidates for the bow, quiver and other gear on her back -- and writes them to
// vrmod_attach_<time>.txt. Ctrl+Alt+H steps through them: the active candidate's matrices get their rotation rows
// zeroed (scale 0) on every game frame and every Present, so a rigid object placed from it would vanish.
bool g_attachScanning = false;
struct AttachCand { uint64_t base; int count; float rel[3]; float dist; };
std::vector<AttachCand> g_attachCands;
std::atomic<uint64_t> g_attachBase{ 0 };
std::atomic<int> g_attachCount{ 0 };
std::atomic<int> g_hideMode{ 0 };   // 0 = off, i = g_attachCands[i - 1]

// ---- Head-bone camera (see bone_scan.h) ----
bool g_locating = false;                        // the running scan is an automatic locate (no report)
std::atomic<uint64_t> g_headBufA{ 0 }, g_headBufB{ 0 };   // Lara's two pose buffers (0 = not located)
std::atomic<int> g_headCount{ 0 };
std::atomic<uint32_t> g_lockGen{ 0 };
std::atomic<bool> g_headLost{ false };
std::atomic<bool> g_hintValid{ false };
std::atomic<float> g_hint[3];
std::atomic<bool> g_headEnabled{ true };
std::atomic<float> g_eyeFwd{ 0.0f }, g_eyeUp{ 0.0f }, g_eyeSide{ 0.0f };
std::atomic<uint32_t> g_stMaxPullMm{0};   // largest pull of the camera back in line with her head (mm, log)
std::atomic<uint64_t> g_stPulls{0};
std::atomic<float> g_nearLiveCm{ 12.0f };
std::atomic<float> g_nearRunLiveCm{ 24.0f };
std::atomic<float> g_speedMps{ 0.0f };
// Standing height (true world scale, vr_overlay.cpp): the head bone's height above the skeleton root, 1 cm bins, sampled
// while she stands still. Its 80th percentile is her standing height (crouching and idle sways sit below it).
std::atomic<uint32_t> g_standBins[256];
std::atomic<uint32_t> g_standSamples{ 0 };      // Lara's horizontal speed (skeleton root), lightly smoothed
std::atomic<uint32_t> g_stMaxSpeedCmps{ 0 };
std::atomic<bool> g_stabOn{ true };
// Stabilisation statistics (game thread -> Present thread): vertical travel of the unstabilised eye vs the one used,
// in 0.1 mm, summed over the stats window.
std::atomic<uint64_t> g_stRawTravel{ 0 }, g_stUsedTravel{ 0 };
std::atomic<uint32_t> g_stRootSteps{ 0 };   // frames whose root step was passed through unsmoothed (ladders)
std::atomic<bool> g_headSettingsLoaded{ false };
double g_lastLocateMs = -1e9;
int g_locateFailures = 0;
// Head-lock cue: three rising tones whenever first person comes on and the head-bone camera is in use -- also when the
// lock from earlier is still good (until 2026-09-30 only a fresh locate beeped, so re-entering first person was silent
// and sounded like a failed lock). A failed lock is announced once instead.
double g_confirmAtMs = 0.0;               // 0 = nothing to confirm
bool g_failAnnounced = false;
std::atomic<double> g_lastEyeMs{ 0.0 };   // last game frame the head bone placed the camera

// Face-guided locate (face_track.cpp): the search starts at her head as the GPU drew it, so it can be tight and needs no
// pose checks (a checkpoint's spawn pose failed them for ~10 s, 2026-10-07). The face point is the face's centre, a few
// cm from bone 12.
constexpr float kFaceLocateCm = 80.0f;
constexpr float kFaceMismatchCm = 60.0f;   // a lock whose head bone is this far from the drawn head is wrong
bool g_faceGuided = false;                 // the running / last locate started at the face point
std::atomic<float> g_faceDistCm{ -1.0f };  // head bone vs drawn head at the last check (log)

float LocateRadiusCm() {
    return kLocateCm + 100.0f * static_cast<float>((std::min)(g_locateFailures, 2));
}
// Statistics (game thread counts, Present thread logs).
std::atomic<uint32_t> g_stOne{ 0 }, g_stBoth{ 0 }, g_stNone{ 0 }, g_stInvalid{ 0 }, g_stFrames{ 0 };
std::atomic<uint32_t> g_stMaxStepMm{ 0 };
std::atomic<int> g_dropReason{ 0 };   // 1 = freshness pattern broke, 2 = repeated jumps (logged by the Present thread)

bool RowOk(const float* r) {
    if (r[3] != 0.0f) return false;
    const float n = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
    return n > 0.25f && n < 4.0f;
}

// m = 16 floats (4x4, translation row last).
bool MatrixOk(const float* m, float maxCm) {
    uint32_t w;
    memcpy(&w, m + 15, 4);
    if (w != kOneBits) return false;
    const float dx = m[12] - Tx, dy = m[13] - Ty, dz = m[14] - Tz;
    if (dx * dx + dy * dy + dz * dz > maxCm * maxCm) return false;
    if (!RowOk(m) || !RowOk(m + 4) || !RowOk(m + 8)) return false;
    const float d01 = m[0] * m[4] + m[1] * m[5] + m[2] * m[6];
    const float d02 = m[0] * m[8] + m[1] * m[9] + m[2] * m[10];
    const float d12 = m[4] * m[8] + m[5] * m[9] + m[6] * m[10];
    return std::fabs(d01) < 0.2f && std::fabs(d02) < 0.2f && std::fabs(d12) < 0.2f;
}

// Shape only (no distance): a rigid-looking bone matrix with translation w = 1.
bool MatrixShapeOk(const float* m) {
    uint32_t w;
    memcpy(&w, m + 15, 4);
    return w == kOneBits && RowOk(m) && RowOk(m + 4) && RowOk(m + 8);
}

void ScanFloats(const uint8_t* p, size_t bytes) {
    // 16-byte aligned 4x4 matrices: test the translation's w (element 15) first, it rejects almost everything.
    const uintptr_t start = (reinterpret_cast<uintptr_t>(p) + 15) & ~uintptr_t(15);
    const uintptr_t end = reinterpret_cast<uintptr_t>(p) + bytes;
    for (uintptr_t a = start; a + 64 <= end; a += 16) {
        const uint32_t* u = reinterpret_cast<const uint32_t*>(a);
        if (u[15] != kOneBits) continue;
        if (!MatrixOk(reinterpret_cast<const float*>(a), g_nearRadiusCm)) continue;
        if (g_hits.size() < kMaxHits) g_hits.push_back(a);
    }
}

bool ScanGuarded(const uint8_t* p, size_t bytes) {
    __try {
        ScanFloats(p, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FindPointers(const uint8_t* p, size_t bytes) {
    const uintptr_t start = (reinterpret_cast<uintptr_t>(p) + 7) & ~uintptr_t(7);
    const uintptr_t end = reinterpret_cast<uintptr_t>(p) + bytes;
    for (uintptr_t a = start; a + 8 <= end; a += 8) {
        const uint64_t v = *reinterpret_cast<const uint64_t*>(a);
        if ((v & 0xF) != 0) continue;
        auto it = g_baseIndex.find(v);
        if (it == g_baseIndex.end()) continue;
        Candidate& c = g_cands[it->second];
        if (c.ptrs.size() >= 16) continue;
        // A skeleton state holds it at +0x70 (or +0x78); its bone count (u16) is at +0x9C.
        if (a + 0x30 > end) continue;   // the count fields would lie past this chunk
        const uint16_t cnt70 = *reinterpret_cast<const uint16_t*>(a - 0x70 + 0x9C);
        const uint16_t cnt78 = *reinterpret_cast<const uint16_t*>(a - 0x78 + 0x9C);
        const bool m70 = cnt70 == c.count, m78 = cnt78 == c.count;
        c.ptrs.push_back({ a, m70 || !m78, m70 ? cnt70 : m78 ? cnt78 : cnt70 });
    }
}

bool PointersGuarded(const uint8_t* p, size_t bytes) {
    __try {
        FindPointers(p, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadMatrixGuarded(uint64_t addr, float out[16]) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Zeroes rows 0-2 of a bone matrix (scale 0), keeping its position row.
void CollapseBoneGuarded(uint64_t addr) {
    __try {
        memset(reinterpret_cast<void*>(addr), 0, 48);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void AttachApply() {
    // DISABLED (2026-09-25): collapsing candidate 2 (a lone transform 46 cm below and behind the head) crashed the game
    // about a second later -- zeroing an arbitrary object's rotation can feed invalid values to game code (physics,
    // inverses) or hit memory the game has freed. Ctrl+Alt+B still lists candidates; nothing is written.
    return;
    const uint64_t base = g_attachBase.load(std::memory_order_relaxed);
    if (!base) return;
    const int n = g_attachCount.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) CollapseBoneGuarded(base + static_cast<uint64_t>(i) * 0x40);
}

bool ReadBytesGuarded(uint64_t addr, void* out, size_t n) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void BuildRegions() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&BuildRegions), &self);
    g_regions.clear();
    g_totalBytes = g_scannedBytes = g_skippedWcBytes = 0;
    g_faulted = 0;
    g_cursor = g_offset = 0;
    uint8_t* p = reinterpret_cast<uint8_t*>(0x10000);
    MEMORY_BASIC_INFORMATION mbi{};
    while (reinterpret_cast<uint64_t>(p) < 0x7FFFFFFF0000ull && VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* next = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        const bool ours = self && mbi.AllocationBase == self;
        // Bone matrices are rewritten every frame, so only writable private/mapped memory is scanned. Write-combined and
        // uncached regions are GPU mappings where CPU reads crawl (the value scan hung the game on them).
        if (mbi.State == MEM_COMMIT && writable && !(mbi.Protect & PAGE_GUARD) && !ours && mbi.Type != MEM_IMAGE) {
            if (mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) g_skippedWcBytes += mbi.RegionSize;
            else { g_regions.push_back({ static_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize }); g_totalBytes += mbi.RegionSize; }
        }
        if (next <= p) break;
        p = next;
    }
    g_startMs = g_lastProgressMs = NowMs();
}

// Runs `fn` over the regions for up to kSliceMs; true when all are done.
template <typename Fn>
bool Slice(Fn fn, const char* what) {
    const double t0 = NowMs();
    while (g_cursor < g_regions.size()) {
        const Region& r = g_regions[g_cursor];
        const size_t n = (std::min)(kChunk, r.size - g_offset);
        if (fn(r.base + g_offset, n)) { g_scannedBytes += n; g_offset += n; }
        else { ++g_faulted; g_offset = r.size; }
        if (g_offset >= r.size) { ++g_cursor; g_offset = 0; }
        if (NowMs() - t0 >= g_sliceMs) break;
    }
    if (NowMs() - g_lastProgressMs > 2000.0) {
        g_lastProgressMs = NowMs();
        modlog::Line(std::string("BONESCAN: ") + what + " " + std::to_string(g_scannedBytes >> 20) + " / " + std::to_string(g_totalBytes >> 20) + " MB");
    }
    return g_cursor >= g_regions.size();
}

// Grows each hit into its array: consecutive valid matrices at 0x40 steps, all within kArrayCm of the in-head point.
void GrowCandidates() {
    std::sort(g_hits.begin(), g_hits.end());
    g_cands.clear();
    g_baseIndex.clear();
    uint64_t coveredEnd = 0;
    for (uint64_t h : g_hits) {
        if (h < coveredEnd || g_cands.size() >= kMaxCandidates) continue;
        float m[16];
        uint64_t b = h, e = h + 0x40;
        int n = 0;
        while (n < kMaxBones && ReadMatrixGuarded(b - 0x40, m) && MatrixOk(m, kArrayCm)) { b -= 0x40; ++n; }
        n = 0;
        while (n < kMaxBones && ReadMatrixGuarded(e, m) && MatrixOk(m, kArrayCm)) { e += 0x40; ++n; }
        Candidate c;
        c.base = b;
        c.count = static_cast<int>((e - b) / 0x40);
        c.seedIndex = static_cast<int>((h - b) / 0x40);
        coveredEnd = e;
        if (c.count < (g_attachScanning ? 1 : 4)) continue;   // a lone matrix is not a skeleton (but may be the bow)
        g_baseIndex[c.base] = g_cands.size();
        g_cands.push_back(std::move(c));
    }
}

// After a locate scan: Lara's skeleton is the pair of equal-length arrays (the pose is double-buffered; the Ctrl+Z
// scans found two 94-bone arrays 0x1BC0 apart, updated on alternate frames) whose head bones agree and sit nearest the
// offset camera (which the user placed just in front of the head). Falls back to the single nearest array.
void ChoosePair() {
    const int H = GetHeadCamSettings().bone;
    struct C { uint64_t base; int count; float head[3]; float d; };
    std::vector<C> cs;
    float m[16];
    // Lara's rig: 94 valid bones from the start of each buffer (Ctrl+Z scans and the first head-camera run). The second
    // run (22:30) locked onto a 361-bone array 137 cm away at the first frame after a load -- hence the shape checks.
    for (const Candidate& c : g_cands) {
        if (c.count <= H || c.count < 80 || c.count > 130) continue;
        if (!ReadMatrixGuarded(c.base + static_cast<uint64_t>(H) * 0x40, m) || !MatrixOk(m, g_nearRadiusCm)) continue;
        float rm[16];
        if (!ReadMatrixGuarded(c.base, rm) || !MatrixShapeOk(rm)) continue;
        // Head above the root (world Z up): standing or crouched. The 2026-09-26 combat run needed 23 s to re-lock after a
        // reload because the old standing-only test (>= 100 cm) failed while Lara was crouched in cover.
        const float up = m[14] - rm[14];
        const float hx = m[12] - rm[12], hy = m[13] - rm[13];
        if (!g_faceGuided && (up < 40.0f || up > 200.0f || hx * hx + hy * hy > 60.0f * 60.0f)) continue;
        const float dx = m[12] - Tx, dy = m[13] - Ty, dz = m[14] - Tz;
        cs.push_back({ c.base, c.count, { m[12], m[13], m[14] }, std::sqrt(dx * dx + dy * dy + dz * dz) });
    }
    int bi = -1, bj = -1;
    float best = 1e30f;
    for (size_t i = 0; i < cs.size(); ++i) {
        for (size_t j = i + 1; j < cs.size(); ++j) {
            if (cs[i].count != cs[j].count) continue;
            const uint64_t gap = cs[i].base > cs[j].base ? cs[i].base - cs[j].base : cs[j].base - cs[i].base;
            if (gap > 0x100000) continue;
            const float dx = cs[i].head[0] - cs[j].head[0], dy = cs[i].head[1] - cs[j].head[1], dz = cs[i].head[2] - cs[j].head[2];
            if (dx * dx + dy * dy + dz * dz > 10.0f * 10.0f) continue;
            const float score = (std::min)(cs[i].d, cs[j].d);
            if (score < best) { best = score; bi = static_cast<int>(i); bj = static_cast<int>(j); }
        }
    }
    // No pair among the candidates: try each Lara-shaped array's known partner directly (the other buffer sits 111
    // slots = 0x1BC0 away). Right after a load the partner can lag a frame or fail the standing test, which cost a 5 s
    // retry (17:32 log: 1 Lara-shaped array, located one retry later).
    uint64_t partner = 0;
    if (bi < 0) {
        for (size_t i = 0; i < cs.size() && !partner; ++i) {
            for (const int64_t step : { int64_t(0x1BC0), -int64_t(0x1BC0) }) {
                const uint64_t pb = cs[i].base + step;
                float pm[16], pr[16];
                if (!ReadMatrixGuarded(pb + static_cast<uint64_t>(H) * 0x40, pm) || !MatrixOk(pm, g_nearRadiusCm)) continue;
                if (!ReadMatrixGuarded(pb, pr) || !MatrixShapeOk(pr)) continue;
                const float dx = pm[12] - cs[i].head[0], dy = pm[13] - cs[i].head[1], dz = pm[14] - cs[i].head[2];
                if (dx * dx + dy * dy + dz * dz > 30.0f * 30.0f) continue;
                bi = static_cast<int>(i);
                partner = pb;
                break;
            }
        }
    }
    // A pair is required: the pose is double-buffered, and a lone array near the camera is more likely something else.
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    if (bi < 0) {
        ++g_locateFailures;
        o << "HEADCAM: Lara's skeleton not found near " << (g_faceGuided ? "her drawn head" : "the first-person camera") << " ("
          << g_cands.size() << " bone arrays looked at; " << cs.size()
          << (g_faceGuided ? " with 80-130 bones" : " Lara-shaped (80-130 bones, head 0.4-2 m above the root)") << " within " << g_nearRadiusCm
          << " cm; a matching PAIR is required). Keeps the offset camera; retrying.";
        // What was there instead (2026-10-01: after a cutscene 64 arrays, none Lara-shaped, for 1.5 min): the larger
        // arrays' bone count, bone H's distance from the hint and height above bone 0.
        std::vector<const Candidate*> big;
        for (const Candidate& c : g_cands) if (c.count >= 40) big.push_back(&c);
        std::sort(big.begin(), big.end(), [](const Candidate* x, const Candidate* y) { return x->count > y->count; });
        if (!big.empty()) {
            o << " Largest arrays:";
            for (size_t i = 0; i < big.size() && i < 8; ++i) {
                const Candidate& c = *big[i];
                o << " [" << c.count << " bones";
                float hm2[16], rm2[16];
                if (c.count > H && ReadMatrixGuarded(c.base + static_cast<uint64_t>(H) * 0x40, hm2) && ReadMatrixGuarded(c.base, rm2)) {
                    const float dx = hm2[12] - Tx, dy = hm2[13] - Ty, dz = hm2[14] - Tz;
                    o << ", bone " << H << " " << std::sqrt(dx * dx + dy * dy + dz * dz) << " cm away, " << hm2[14] - rm2[14] << " cm above bone 0";
                }
                o << "]";
            }
        }
        modlog::Line(o.str());
        if (g_locateFailures >= 3 && !g_failAnnounced && IsFirstPersonEnabled()) {
            g_failAnnounced = true;
            g_confirmAtMs = 0.0;
            if (VRMOD_DIAG) PlayCue(Cue::Failure);   // development signal; the log says it
        }
        return;
    }
    g_locateFailures = 0;
    g_headCount.store(cs[bi].count);
    if (partner) best = cs[bi].d;
    const uint64_t other = bj >= 0 ? cs[bj].base : partner;
    g_headBufB.store(other);
    g_headBufA.store(cs[bi].base);
    g_lockGen.fetch_add(1);
    o << "HEADCAM: skeleton located -- pose buffers " << "0x" << std::hex << cs[bi].base;
    if (other) o << " and 0x" << other << (partner ? " (partner at +-0x1BC0)" : "");
    o << std::dec << " (" << cs[bi].count << " bones), head bone " << H << " " << best << " cm from "
      << (g_faceGuided ? "her drawn head (face-guided)" : "the offset camera")
      << (other ? "" : "; only ONE buffer found") << ".";
    modlog::Line(o.str());
    if (VRMOD_DIAG) PlayCue(Cue::Success);   // development signal (every load in play); the log says it
    g_confirmAtMs = 0.0;   // this is the confirmation
    g_failAnnounced = false;
    // What the head region and the slots after the valid run hold (for hiding the head: face bones may live there).
    float hm[16];
    if (ReadMatrixGuarded(cs[bi].base + static_cast<uint64_t>(H) * 0x40, hm)) {
        std::ostringstream d;
        d.setf(std::ios::fixed);
        d.precision(2);
        d << "HEADCAM bones (position relative to bone " << H << " in cm; row lengths; w):";
        auto slot = [&](int i) {
            float m[16];
            if (!ReadMatrixGuarded(cs[bi].base + static_cast<uint64_t>(i) * 0x40, m)) { d << "\n  " << i << ": unreadable"; return; }
            auto len = [&](int r) { return std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]); };
            d << "\n  " << i << ": (" << m[12] - hm[12] << ", " << m[13] - hm[13] << ", " << m[14] - hm[14] << ") rows " << len(0) << " "
              << len(1) << " " << len(2) << " w " << m[3] << " " << m[7] << " " << m[11] << " " << m[15];
        };
        for (int i = 8; i <= 20; ++i) slot(i);
        for (int i = cs[bi].count - 2; i < cs[bi].count + 20; ++i) slot(i);
        modlog::Line(d.str());
    }
}

void ToLocal(float dx, float dy, float dz, float out[3]);
std::string Stamp();

// After Ctrl+Alt+B: keep arrays that aren't Lara's pose buffers and have a transform behind the head (camera axes).
void FinishAttachScan() {
    const uint64_t A = g_headBufA.load(), B = g_headBufB.load();
    const uint64_t spanA0 = A, spanA1 = A + 111 * 0x40, spanB0 = B, spanB1 = B + 111 * 0x40;
    g_attachCands.clear();
    float m[16];
    for (const Candidate& c : g_cands) {
        const uint64_t e = c.base + static_cast<uint64_t>(c.count) * 0x40;
        if ((A && c.base < spanA1 && e > spanA0) || (B && c.base < spanB1 && e > spanB0)) continue;   // Lara's skeleton
        AttachCand best{ c.base, c.count, { 0, 0, 0 }, 1e30f };
        for (int i = 0; i < c.count && i < 64; ++i) {
            if (!ReadMatrixGuarded(c.base + static_cast<uint64_t>(i) * 0x40, m)) break;
            float l[3];
            ToLocal(m[12] - Tx, m[13] - Ty, m[14] - Tz, l);
            const float d = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (d < kNearRadiusAttach && l[1] < -3.0f && d < best.dist) { best.dist = d; best.rel[0] = l[0]; best.rel[1] = l[1]; best.rel[2] = l[2]; }
        }
        if (best.dist < 1e29f) g_attachCands.push_back(best);
    }
    std::sort(g_attachCands.begin(), g_attachCands.end(), [](const AttachCand& a, const AttachCand& b) { return a.dist < b.dist; });
    if (g_attachCands.size() > 60) g_attachCands.resize(60);
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    o << "Bow/quiver finder " << Stamp() << ": transforms within " << kNearRadiusAttach << " cm of the head bone and behind it (camera axes: right, forward, up cm),\n"
      << "not in Lara's pose buffers; Ctrl+Alt+H order.\n";
    for (size_t i = 0; i < g_attachCands.size(); ++i) {
        const AttachCand& c = g_attachCands[i];
        o << "  " << i + 1 << ": 0x" << std::hex << c.base << std::dec << "  " << c.count << " matrices; nearest at right " << c.rel[0] << " fwd "
          << c.rel[1] << " up " << c.rel[2] << " (" << c.dist << " cm)\n";
    }
    const std::string name = "vrmod_attach_" + Stamp() + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();
    g_hideMode.store(0);
    g_attachBase.store(0);
    modlog::Line("BOWFINDER: " + std::to_string(g_attachCands.size()) + " candidate transforms behind the head -> " + name
        + "; Ctrl+Alt+H steps through them.");
    PlayCue(g_attachCands.empty() ? Cue::Failure : Cue::Success);
}

void SampleOnce() {
    float m[16];
    for (Candidate& c : g_cands) {
        const int n = (std::min)(c.count, kMaxBones);
        if (c.first.empty()) { c.first.assign(n * 3, 0.0f); c.lo.assign(n * 3, 1e30f); c.hi.assign(n * 3, -1e30f); c.last.assign(n * 3, 0.0f); }
        bool changed = false, ok = true;
        for (int i = 0; i < n; ++i) {
            if (!ReadMatrixGuarded(c.base + static_cast<uint64_t>(i) * 0x40, m)) { ok = false; break; }
            for (int k = 0; k < 3; ++k) {
                const float v = m[12 + k];
                if (c.samples == 0) c.first[i * 3 + k] = v;
                else if (v != c.last[i * 3 + k]) changed = true;
                c.last[i * 3 + k] = v;
                c.lo[i * 3 + k] = (std::min)(c.lo[i * 3 + k], v);
                c.hi[i * 3 + k] = (std::max)(c.hi[i * 3 + k], v);
            }
        }
        if (!ok) continue;
        if (c.samples > 0 && changed) ++c.changes;
        ++c.samples;
    }
}

// World offset -> camera-local (right, forward, up) with the rotation at Ctrl+Z: conj(q) v q.
void ToLocal(float dx, float dy, float dz, float out[3]) {
    const float qx = -Tq[0], qy = -Tq[1], qz = -Tq[2], qw = Tq[3];
    const float tx = 2.0f * (qy * dz - qz * dy), ty = 2.0f * (qz * dx - qx * dz), tz = 2.0f * (qx * dy - qy * dx);
    out[0] = dx + qw * tx + (qy * tz - qz * ty);
    out[1] = dy + qw * ty + (qz * tx - qx * tz);
    out[2] = dz + qw * tz + (qx * ty - qy * tx);
}

std::string Hex(uint64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

std::string Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void Report() {
    struct Scored { Candidate* c; int nearest; float nearestCm; bool confirmed; };
    std::vector<Scored> order;
    for (Candidate& c : g_cands) {
        if (c.samples == 0) continue;
        int best = -1;
        float bestD = 1e30f;
        for (int i = 0; i < (std::min)(c.count, kMaxBones); ++i) {
            const float dx = c.last[i * 3] - Tx, dy = c.last[i * 3 + 1] - Ty, dz = c.last[i * 3 + 2] - Tz;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < bestD) { bestD = d; best = i; }
        }
        bool confirmed = false;
        for (const PtrHit& p : c.ptrs) if (p.countField == c.count) confirmed = true;
        order.push_back({ &c, best, bestD, confirmed });
    }
    std::stable_sort(order.begin(), order.end(), [](const Scored& a, const Scored& b) {
        if (a.confirmed != b.confirmed) return a.confirmed;
        const bool la = a.c->changes * 2 >= a.c->samples, lb = b.c->changes * 2 >= b.c->samples;
        if (la != lb) return la;
        return a.nearestCm < b.nearestCm;
    });

    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    o << "Bone scan " << Stamp() << "\n"
      << "in-head point (camera at Ctrl+Z) " << Tx << " " << Ty << " " << Tz << " cm; camera quat " << Tq[0] << " " << Tq[1] << " "
      << Tq[2] << " " << Tq[3] << "; first person " << (IsFirstPersonEnabled() ? "on" : "OFF") << "\n"
      << "scanned " << (g_totalBytes >> 20) << " MB writable memory (skipped " << (g_skippedWcBytes >> 20) << " MB write-combined), "
      << g_faulted << " faulted; matrix scan " << g_scanMs << " ms, pointer scan " << g_ptrMs << " ms; " << g_hits.size()
      << " matrices within " << kNearCm << " cm; " << g_cands.size() << " arrays (>= 4 matrices)\n"
      << "Per array: 'confirmed' = a pointer to it sits at a skeleton state's +0x70/+0x78 whose u16 at +0x9C equals the array\n"
      << "length (the Ghidra layout). 'updates' = Presents where any bone moved / samples. Bones: index, position relative to\n"
      << "the in-head point in camera axes (right, forward, up; cm), distance, and how far it moved during sampling (cm).\n\n";
    int listed = 0;
    for (const Scored& s : order) {
        const Candidate& c = *s.c;
        o << "ARRAY " << Hex(c.base) << "  bones " << c.count << "  updates " << c.changes << "/" << (c.samples - 1)
          << "  nearest bone " << s.nearest << " at " << s.nearestCm << " cm" << (s.confirmed ? "  CONFIRMED" : "") << "\n";
        for (const PtrHit& p : c.ptrs) {
            const uint64_t state = p.at - (p.viaDefault ? 0x70 : 0x78);
            o << "   pointer at " << Hex(p.at) << " -> state " << Hex(state) << " (+0x" << (p.viaDefault ? "70" : "78")
              << "), u16 +0x9C = " << p.countField << (p.countField == c.count ? " (matches)" : "") << "\n";
        }
        if (listed < 4) {
            for (int i = 0; i < (std::min)(c.count, kMaxBones); ++i) {
                float l[3];
                ToLocal(c.last[i * 3] - Tx, c.last[i * 3 + 1] - Ty, c.last[i * 3 + 2] - Tz, l);
                const float d = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                const float mx = c.hi[i * 3] - c.lo[i * 3], my = c.hi[i * 3 + 1] - c.lo[i * 3 + 1], mz = c.hi[i * 3 + 2] - c.lo[i * 3 + 2];
                char line[160];
                snprintf(line, sizeof(line), "   bone %3d  right %7.1f  fwd %7.1f  up %7.1f  dist %6.1f  moved %5.1f\n", i, l[0], l[1], l[2], d,
                         std::sqrt(mx * mx + my * my + mz * mz));
                o << line;
            }
        }
        ++listed;
    }
    const std::string name = "vrmod_bonescan_" + Stamp() + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();
    int confirmed = 0;
    for (const Scored& s : order) if (s.confirmed) ++confirmed;
    modlog::Line("BONESCAN: " + std::to_string(g_cands.size()) + " bone arrays near the in-head point, " + std::to_string(confirmed)
        + " confirmed by the skeleton layout -> " + name);
    PlayCue(confirmed > 0 ? Cue::Success : Cue::Failure);
}

#ifdef BONE_SCAN_HARNESS
bool g_forceStart = false;
#endif

bool KeyZ(bool wantAlt) {
#ifdef BONE_SCAN_HARNESS
    if (!wantAlt && g_forceStart) { g_forceStart = false; return true; }
    if (!wantAlt) return false;
#endif
    static bool was[2] = { false, false };
    const bool down = (VRMOD_DEV_KEY('Z') & 0x8000) != 0;
    bool& w = was[wantAlt ? 1 : 0];
    const bool pressed = down && !w;
    w = down;
    if (!pressed) return false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return ctrl && alt == wantAlt;
}

} // namespace

#ifdef BONE_SCAN_HARNESS
void BoneScanForceStart() { g_forceStart = true; }
bool BoneScanIdle() { return g_phase == Idle; }
#endif

namespace {

void LoadHeadSettingsOnce() {
    if (g_headSettingsLoaded.exchange(true)) return;
    const HeadCamSettings& h = GetHeadCamSettings();
    g_headEnabled.store(h.enabled);
    g_eyeFwd.store(h.eyeFwdCm);
    g_eyeUp.store(h.eyeUpCm);
    g_eyeSide.store(h.eyeSideCm);
    g_nearLiveCm.store(h.nearCm);
    g_nearRunLiveCm.store(h.nearRunCm);
    g_stabOn.store(h.stabilise);
}

void SaveHeadLive() {
    HeadCamSettings h = GetHeadCamSettings();
    h.enabled = g_headEnabled.load();
    h.eyeFwdCm = g_eyeFwd.load();
    h.eyeUpCm = g_eyeUp.load();
    h.eyeSideCm = g_eyeSide.load();
    h.nearCm = g_nearLiveCm.load();
    h.nearRunCm = g_nearRunLiveCm.load();
    h.stabilise = g_stabOn.load();
    SetHeadCamForSave(h);
}

// Present thread, every frame: a statistics line every ~10 s while the head-bone camera is in use.
void HeadCamPresent() {
    LoadHeadSettingsOnce();
    if (const int why = g_dropReason.exchange(0)) {
        modlog::Line(std::string("HEADCAM: lock DROPPED -- ") + (why == 2 ? "the head jumped over 1.5 m between frames repeatedly"
            : "unreadable pose data or both buffers changing at once") + "; offset camera until Lara's skeleton is found again.");
        if (VRMOD_DIAG) PlayCue(Cue::Failure);   // development signal; the log says it
    }
    {
        // Head-lock cue (see g_confirmAtMs). Waits 400 ms so the first-person "on" tones finish first.
        static bool fpWas = false;
        const bool fp = IsFirstPersonEnabled();
        if (fp && !fpWas) { g_confirmAtMs = NowMs() + 400.0; g_failAnnounced = false; }
        if (!fp) g_confirmAtMs = 0.0;
        fpWas = fp;
        if (g_confirmAtMs > 0.0 && NowMs() >= g_confirmAtMs && HeadBoneActive() && NowMs() - g_lastEyeMs.load() < 250.0) {
            g_confirmAtMs = 0.0;
            modlog::Line("HEADCAM: first person on -- head-bone camera in use (lock kept from earlier).");
            if (GetViewCuesSetting()) PlayCue(Cue::Success);   // a view switch: silent unless [sound] view_cues=1
        }
    }
    AttachApply();   // also from the Present thread (the object may be placed at either end of the frame)
    {
        static bool hWas = false, bWas = false;
        const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
        const bool hDown = (VRMOD_DEV_KEY('H') & 0x8000) != 0, bDown = (VRMOD_DEV_KEY('B') & 0x8000) != 0;
        if (bDown && !bWas && ca) {
            const uint64_t A = g_headBufA.load();
            float hm[16];
            CamPose pose{};
            if (g_phase != Idle || !A || !ReadMatrixGuarded(A + static_cast<uint64_t>(GetHeadCamSettings().bone) * 0x40, hm)
                || !CameraReadStructPose(&pose)) {
                modlog::Line("BOWFINDER: needs the head camera locked (first person on) and no scan running.");
                PlayCue(Cue::Failure);
            } else {
                g_hideMode.store(0);
                g_attachBase.store(0);
                Tx = hm[12]; Ty = hm[13]; Tz = hm[14];
                for (int k = 0; k < 4; ++k) Tq[k] = pose.q[k];
                g_hits.clear();
                BuildRegions();
                g_attachScanning = true;
                g_nearRadiusCm = kNearRadiusAttach;
                g_sliceMs = kSliceMs;
                g_phase = Scan;
                modlog::Line("BOWFINDER: scanning for transforms near the back of the head...");
                PlayCue(Cue::On);
            }
        }
        if (hDown && !hWas && ca) {
            if (g_attachCands.empty()) {
                modlog::Line("BOWFINDER: scan first (Ctrl+Alt+B).");
                PlayCue(Cue::Failure);
            } else {
                const int next = (g_hideMode.load() + 1) % (static_cast<int>(g_attachCands.size()) + 1);
                g_hideMode.store(next);
                std::ostringstream o;
                o.setf(std::ios::fixed);
                o.precision(1);
                if (next == 0) {
                    g_attachBase.store(0);
                    o << "BOWFINDER: OFF";
                } else {
                    const AttachCand& c = g_attachCands[next - 1];
                    g_attachCount.store((std::min)(c.count, 64));
                    g_attachBase.store(c.base);
                    o << "BOWFINDER: candidate " << next << "/" << g_attachCands.size() << " collapsed (0x" << std::hex << c.base << std::dec
                      << ", " << c.count << " matrices, nearest right " << c.rel[0] << " fwd " << c.rel[1] << " up " << c.rel[2] << " cm)";
                }
                modlog::Line(o.str());
                PlayCue(next ? Cue::On : Cue::Off);
            }
        }
        hWas = hDown;
        bWas = bDown;
        static bool sWas = false;
        const bool sDown = (VRMOD_DEV_KEY('S') & 0x8000) != 0;
        if (sDown && !sWas && ca) HeadBoneToggleStabilise();
        sWas = sDown;
    }
    {
        // Check the lock against her drawn head once a second: five misses in a row (> kFaceMismatchCm) mean the lock is
        // on someone or something else -- locate again.
        static double lastCheck = 0.0;
        static int misses = 0;
        const uint64_t A = g_headBufA.load();
        float face[3], hm[16];
        if (NowMs() - lastCheck > 1000.0) {
            lastCheck = NowMs();
            if (A && !g_headLost.load() && g_phase == Idle && FaceHeadPoint(face, 300.0)
                && ReadMatrixGuarded(A + static_cast<uint64_t>(GetHeadCamSettings().bone) * 0x40, hm)) {
                const float dx = hm[12] - face[0], dy = hm[13] - face[1], dz = hm[14] - face[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                g_faceDistCm.store(d);
                misses = d > kFaceMismatchCm ? misses + 1 : 0;
                if (misses >= 5) {
                    misses = 0;
                    char b[160];
                    snprintf(b, sizeof(b), "the locked head bone is %.0f cm from her drawn head", d);
                    HeadBoneForget(b);
                }
            }
        }
    }
    static double lastLog = 0.0;
    const double now = NowMs();
    if (now - lastLog < 10000.0) return;
    lastLog = now;
    modlog::Line("FACETRACK (10 s): " + FaceTrackStats() + "; locked head bone vs drawn head "
        + (g_faceDistCm.load() < 0.0f ? std::string("n/a") : std::to_string(static_cast<int>(g_faceDistCm.load() + 0.5f)) + " cm"));
    g_faceDistCm.store(-1.0f);
    const uint32_t frames = g_stFrames.exchange(0);
    if (frames == 0 || g_headBufA.load() == 0) return;
    std::ostringstream o;
    o << "HEADCAM (last " << frames << " game frames): fresh buffer by change -- exactly one " << g_stOne.exchange(0) << ", both "
      << g_stBoth.exchange(0) << ", neither " << g_stNone.exchange(0) << "; unreadable " << g_stInvalid.exchange(0)
      << "; largest camera step between frames " << g_stMaxStepMm.exchange(0) / 10.0f << " cm; pulled back in line with her head in "
      << g_stPulls.exchange(0) << " frames (largest " << g_stMaxPullMm.exchange(0) / 10.0f << " cm); root steps passed through "
      << g_stRootSteps.exchange(0)
      << " | eye offset fwd " << g_eyeFwd.load()
      << " up " << g_eyeUp.load() << " side " << g_eyeSide.load() << " cm, smoothing " << GetHeadCamSettings().smoothMs << " ms";
    const HeadCamSettings& hs = GetHeadCamSettings();
    o.setf(std::ios::fixed);
    o.precision(1);
    o << " | stabilise " << (g_stabOn.load() ? "ON" : "off") << " (band " << hs.stabVerticalCm << " cm vertical, " << hs.stabHorizontalCm
      << " cm horizontal, " << hs.stabSmoothMs << " ms): vertical travel of the head " << g_stRawTravel.exchange(0) / 100.0f
      << " cm, of the camera " << g_stUsedTravel.exchange(0) / 100.0f << " cm | speed now " << g_speedMps.load() << " m/s, max "
      << g_stMaxSpeedCmps.exchange(0) / 100.0f << " m/s -> near clip now " << HeadCamNearCm() << " cm (standing " << g_nearLiveCm.load()
      << ", running " << g_nearRunLiveCm.load() << ")";
    uint32_t standN = 0;
    const float standCm = HeadBoneStandingHeightCm(&standN);
    o << " | head bone above the root standing still: " << standCm << " cm (80th percentile of " << standN << " samples)";
    modlog::Line(o.str());
}

} // namespace

void HeadBonePublishHint(const float pos[3]) {
    for (int k = 0; k < 3; ++k) g_hint[k].store(pos[k], std::memory_order_relaxed);
    g_hintValid.store(true, std::memory_order_release);
}

bool HeadBoneActive() {
    return g_headEnabled.load() && g_headBufA.load() != 0 && !g_headLost.load();
}

void HeadBoneForget(const char* why) {
    if (g_headBufA.load() == 0) return;
    g_headLost.store(true);
    modlog::Line(std::string("HEADCAM: locating Lara's skeleton again (") + why + ")");
}

float HeadBoneStandingHeightCm(uint32_t* samples) {
    const uint32_t n = g_standSamples.load(std::memory_order_relaxed);
    if (samples) *samples = n;
    if (n < 300) return 0.0f;
    uint32_t acc = 0;
    for (int i = 0; i < 256; ++i) {
        acc += g_standBins[i].load(std::memory_order_relaxed);
        if (acc >= n * 8 / 10) return static_cast<float>(i) + 0.5f;
    }
    return 0.0f;
}

bool HeadBoneStabiliseOn() {
    LoadHeadSettingsOnce();
    return g_stabOn.load(std::memory_order_relaxed);
}

float HeadBoneEyeForwardCm() {
    LoadHeadSettingsOnce();
    return g_eyeFwd.load(std::memory_order_relaxed);
}

void HeadBoneToggleStabilise() {
    LoadHeadSettingsOnce();
    const bool on = !g_stabOn.load();
    g_stabOn.store(on);
    SaveHeadLive();
    modlog::Line(std::string("HEADCAM: stabilisation ") + (on ? "ON" : "off") + " (gamepad LB + left stick click / Ctrl+Alt+S)");
    PlayCue(on ? Cue::On : Cue::Off);
}

void HeadBoneToggle() {
    LoadHeadSettingsOnce();
    const bool on = !g_headEnabled.load();
    g_headEnabled.store(on);
    SaveHeadLive();
    modlog::Line(std::string("HEADCAM: head-bone camera ") + (on ? "ON" : "off (offset camera)"));
    PlayCue(on ? Cue::On : Cue::Off);
}

namespace {
// 0 standing still .. 1 running, from her speed.
float RunFactor() {
    const HeadCamSettings& h = GetHeadCamSettings();
    const float v = g_speedMps.load(std::memory_order_relaxed);
    const float f = (v - h.nearRunFromMps) / (h.nearRunFullMps - h.nearRunFromMps);
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}
}

float HeadCamNearCm() {
    LoadHeadSettingsOnce();
    const float still = g_nearLiveCm.load(std::memory_order_relaxed), run = g_nearRunLiveCm.load(std::memory_order_relaxed);
    return still + (run - still) * RunFactor();
}

void HeadCamAdjustNear(float dCm) {
    LoadHeadSettingsOnce();
    // Adjusts the value in use: the running one while she runs, the standing one otherwise.
    const bool running = RunFactor() >= 0.5f;
    std::atomic<float>& target = running ? g_nearRunLiveCm : g_nearLiveCm;
    float v = target.load() + dCm;
    v = v < 1.0f ? 1.0f : (v > 60.0f ? 60.0f : v);
    target.store(v);
    SaveHeadLive();
    std::ostringstream o;
    o << "HEADCAM: first-person near clip " << (running ? "running " : "standing ") << v << " cm (standing " << g_nearLiveCm.load()
      << ", running " << g_nearRunLiveCm.load() << ")";
    modlog::Line(o.str());
}

void HeadBoneAdjustEye(float dFwdCm, float dUpCm, float dSideCm) {
    LoadHeadSettingsOnce();
    auto clampTo = [](float v) { return v < -50.0f ? -50.0f : (v > 50.0f ? 50.0f : v); };
    g_eyeFwd.store(clampTo(g_eyeFwd.load() + dFwdCm));
    g_eyeUp.store(clampTo(g_eyeUp.load() + dUpCm));
    g_eyeSide.store(clampTo(g_eyeSide.load() + dSideCm));
    SaveHeadLive();
    std::ostringstream o;
    o << "HEADCAM: eye offset fwd " << g_eyeFwd.load() << " up " << g_eyeUp.load() << " side " << g_eyeSide.load() << " cm";
    modlog::Line(o.str());
}

// Game thread (camera hook). Reads the head bone and the root from both pose buffers once per game frame; the fresh
// buffer is the one whose head changed since the previous game frame. The head's offset from the root is smoothed; the
// root carries locomotion unsmoothed, so running doesn't make the camera lag behind the head.
bool HeadBoneEye(uint64_t gameFrame, const float q[4], float out[3]) {
    if (!g_headEnabled.load(std::memory_order_relaxed)) return false;
    const uint64_t A = g_headBufA.load(std::memory_order_relaxed), B = g_headBufB.load(std::memory_order_relaxed);
    if (!A || g_headLost.load(std::memory_order_relaxed)) return false;
    static uint64_t lastFrame = ~0ull, lastA = 0;
    static float cached[3];
    static bool cachedOk = false;
    static float lastHead[2][3], sm[3], lastUsed[3];
    static float follow[3], lastRawZ = 0.0f;   // stabilisation: the followed offset; statistics
    static bool haveFollow = false;
    static bool haveLast = false, haveSm = false;
    static int fresh = 0, invalidRun = 0;
    static LONGLONG lastQpc = 0;
    static bool haveUsed = false;
    static uint32_t lastGen = 0;
    static LONGLONG lastCallQpc = 0;
    // Start fresh on a new lock -- even of the same buffers: run 8 re-found the same pair after each drop, kept the old
    // position, rejected every frame as a jump and dropped again every second -- and when the camera wasn't used for a
    // while (first person was off: Lara may be somewhere else entirely by now).
    {
        LARGE_INTEGER nowQ, fq;
        QueryPerformanceCounter(&nowQ);
        QueryPerformanceFrequency(&fq);
        const bool gap = lastCallQpc != 0 && (nowQ.QuadPart - lastCallQpc) > fq.QuadPart / 4;   // 250 ms
        lastCallQpc = nowQ.QuadPart;
        const uint32_t gen = g_lockGen.load(std::memory_order_relaxed);
        if (A != lastA || gen != lastGen || gap) {
            lastA = A;
            lastGen = gen;
            haveLast = haveSm = cachedOk = haveUsed = haveFollow = false;
            invalidRun = 0;
            lastFrame = ~0ull;
            lastQpc = 0;
        }
    }
    if (gameFrame == lastFrame) {   // the second write of this frame: same answer
        if (cachedOk) for (int k = 0; k < 3; ++k) out[k] = cached[k];
        return cachedOk;
    }
    lastFrame = gameFrame;
    g_stFrames.fetch_add(1, std::memory_order_relaxed);

    const int H = GetHeadCamSettings().bone;
    float head[2][3] = {}, root[2][3] = {};
    bool ok[2] = { false, false };
    for (int b = 0; b < 2; ++b) {
        const uint64_t buf = b == 0 ? A : B;
        if (!buf) continue;
        float mh[16], mr[16];
        if (!ReadMatrixGuarded(buf + static_cast<uint64_t>(H) * 0x40, mh) || !ReadMatrixGuarded(buf, mr)) continue;
        if (!MatrixShapeOk(mh) || !MatrixShapeOk(mr)) continue;
        const float dx = mh[12] - mr[12], dy = mh[13] - mr[13], dz = mh[14] - mr[14];
        if (dx * dx + dy * dy + dz * dz > 250.0f * 250.0f) continue;
        for (int k = 0; k < 3; ++k) { head[b][k] = mh[12 + k]; root[b][k] = mr[12 + k]; }
        ok[b] = true;
    }
    // Health: in a good lock exactly one buffer changes every game frame (720 of 720 in the first run). A window of 72
    // frames with fewer than 54 such frames, or 3 jumps over 150 cm, drops the lock (offset camera; locate again).
    // Run 7 dropped a good lock when the game paused Lara's animation for 27 frames ("neither" buffer changed): valid,
    // unchanged data. So only unreadable frames and "both changed" count against the lock (the wrong 361-bone lock
    // of run 3 had plenty of both).
    static int winFrames = 0, winBad = 0, winJumps = 0;
    if (!haveLast && !haveUsed) winFrames = winBad = winJumps = 0;   // a fresh start (see above)
    auto endWindow = [&]() {
        if (++winFrames < 72) return false;
        const bool bad = winBad >= 12 || winJumps >= 3;
        if (bad) g_dropReason.store(winJumps >= 3 ? 2 : 1);
        winFrames = winBad = winJumps = 0;
        if (bad) { g_headLost.store(true); cachedOk = false; }
        return bad;
    };
    if (!ok[0] && !ok[1]) {
        g_stInvalid.fetch_add(1, std::memory_order_relaxed);
        if (++invalidRun > 36) g_headLost.store(true);   // half a second unreadable: re-locate (level load, respawn)
        ++winBad;
        endWindow();
        cachedOk = false;
        return false;
    }
    invalidRun = 0;
    bool ch[2] = { false, false };
    for (int b = 0; b < 2; ++b) {
        ch[b] = ok[b] && haveLast && (head[b][0] != lastHead[b][0] || head[b][1] != lastHead[b][1] || head[b][2] != lastHead[b][2]);
    }
    if (!haveLast) fresh = ok[0] ? 0 : 1;
    else if (ch[0] != ch[1]) { fresh = ch[0] ? 0 : 1; g_stOne.fetch_add(1, std::memory_order_relaxed); }
    else if (ch[0] && ch[1]) { fresh = 1 - fresh; g_stBoth.fetch_add(1, std::memory_order_relaxed); ++winBad; }
    else g_stNone.fetch_add(1, std::memory_order_relaxed);
    if (!ok[fresh]) fresh = 1 - fresh;
    for (int b = 0; b < 2; ++b) for (int k = 0; k < 3; ++k) lastHead[b][k] = head[b][k];
    haveLast = true;
    AttachApply();

    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    double dt = lastQpc ? static_cast<double>(t.QuadPart - lastQpc) / static_cast<double>(f.QuadPart) : 0.0;
    lastQpc = t.QuadPart;
    if (dt > 0.1) dt = 0.1;
    {
        // Her horizontal speed from the skeleton root (locomotion), smoothed over 80 ms: she accelerates almost at once.
        static float lastRoot[2] = { 0, 0 };
        static bool haveRoot = false;
        static uint32_t lastGen = ~0u;
        const uint32_t gen = g_lockGen.load(std::memory_order_relaxed);
        if (gen != lastGen) { lastGen = gen; haveRoot = false; }
        const float rx = root[fresh][0], ry = root[fresh][1];
        if (haveRoot && dt > 0.0) {
            const float sx = rx - lastRoot[0], sy = ry - lastRoot[1];
            const float step = std::sqrt(sx * sx + sy * sy);
            if (step < 150.0f) {   // a teleport isn't speed
                const float inst = static_cast<float>(step / dt) / 100.0f;   // cm per s -> m/s
                const float a = static_cast<float>(1.0 - std::exp(-dt / 0.08));
                float v = g_speedMps.load(std::memory_order_relaxed);
                v += a * (inst - v);
                g_speedMps.store(v, std::memory_order_relaxed);
                const uint32_t cm = static_cast<uint32_t>(v * 100.0f);
                uint32_t prev = g_stMaxSpeedCmps.load(std::memory_order_relaxed);
                while (cm > prev && !g_stMaxSpeedCmps.compare_exchange_weak(prev, cm, std::memory_order_relaxed)) {}
            }
        }
        lastRoot[0] = rx;
        lastRoot[1] = ry;
        haveRoot = true;
    }
    const HeadCamSettings& hs = GetHeadCamSettings();
    const bool stab = g_stabOn.load(std::memory_order_relaxed);
    // Running (2026-10-27, "judder when running"): her body (skeleton root) moves forward evenly -- 3.8 % variation of the
    // per-frame step -- but her head sways fore and aft with the stride (12 %), and the camera showed 9 %. With the same
    // 0..1 running factor as the near clip (nearRunFromMps .. nearRunFullMps), the stabiliser holds the head's horizontal
    // sway harder -- band 6 -> 15 cm, smoothing 150 -> 300 ms -- and the camera may sit further from her head (below):
    // the 48 cm near clip hides her then. Replayed on the head log: 9.1 % -> 5.1 %.
    const float speedNow = g_speedMps.load(std::memory_order_relaxed);
    const float run = hs.nearRunFullMps > hs.nearRunFromMps
                          ? std::clamp((speedNow - hs.nearRunFromMps) / (hs.nearRunFullMps - hs.nearRunFromMps), 0.0f, 1.0f) : 0.0f;
    const float tau = (stab ? hs.stabSmoothMs : hs.smoothMs) / 1000.0f;
    const float tauH = stab ? tau + run * (0.300f - tau > 0.0f ? 0.300f - tau : 0.0f) : tau;
    const float a = (tau > 0.0f && haveSm) ? static_cast<float>(1.0 - std::exp(-dt / tau)) : 1.0f;
    const float aH = (tauH > 0.0f && haveSm) ? static_cast<float>(1.0 - std::exp(-dt / tauH)) : 1.0f;
    float off[3];
    for (int k = 0; k < 3; ++k) off[k] = head[fresh][k] - root[fresh][k];
    // The offset jumping faster than a head moves on its own body (4 m/s) is the skeleton's root being placed anew, not her
    // head moving: on a ladder the root steps ~75 cm up at each rung while the head climbs smoothly (head log,
    // 2026-10-27). Passed straight through to the smoothed and followed offsets, so the camera keeps following the head;
    // smoothed, the root's step reached the camera at once and the offset's matching drop only later -- the camera shot up
    // to 43 cm above her head and sank back at every rung ("two steps forward, one back").
    {
        static float lastOff[3];
        static bool haveLastOff = false;
        if (haveLastOff && haveSm && dt > 0.0) {
            const float d[3] = { off[0] - lastOff[0], off[1] - lastOff[1], off[2] - lastOff[2] };
            const float dn = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dn > 400.0f * static_cast<float>(dt) && dn < 150.0f) {
                for (int k = 0; k < 3; ++k) {
                    sm[k] += d[k];
                    if (haveFollow) follow[k] += d[k];
                }
                g_stRootSteps.fetch_add(1, std::memory_order_relaxed);
            }
        }
        for (int k = 0; k < 3; ++k) lastOff[k] = off[k];
        haveLastOff = true;
    }
    // Stabilisation: the offset the smoothing follows moves only when the head leaves the band around it -- up/down
    // separately from the horizontal plane (fore-aft and sideways together, so turning around doesn't count twice).
    float target[3] = { off[0], off[1], off[2] };
    if (stab) {
        if (!haveFollow) {
            for (int k = 0; k < 3; ++k) follow[k] = off[k];
            haveFollow = true;
        }
        const float ez = off[2] - follow[2], bv = hs.stabVerticalCm;
        if (ez > bv) follow[2] = off[2] - bv;
        else if (ez < -bv) follow[2] = off[2] + bv;
        const float bh = hs.stabHorizontalCm + run * (std::max)(0.0f, 15.0f - hs.stabHorizontalCm);
        const float ex = off[0] - follow[0], ey = off[1] - follow[1], eh = std::sqrt(ex * ex + ey * ey);
        if (eh > bh) {
            const float move = (eh - bh) / eh;
            follow[0] += ex * move;
            follow[1] += ey * move;
        }
        for (int k = 0; k < 3; ++k) target[k] = follow[k];
    } else {
        haveFollow = false;
    }
    const float jx = target[0] - sm[0], jy = target[1] - sm[1], jz = target[2] - sm[2];
    const bool restart = !haveSm || jx * jx + jy * jy + jz * jz > 100.0f * 100.0f;
    if (restart) {
        for (int k = 0; k < 3; ++k) sm[k] = target[k];
        haveSm = true;
    } else {
        for (int k = 0; k < 3; ++k) sm[k] += (k < 2 ? aH : a) * (target[k] - sm[k]);
    }
    if (g_speedMps.load(std::memory_order_relaxed) < 0.05f && off[2] > 0.0f && off[2] < 255.0f) {
        g_standBins[static_cast<int>(off[2])].fetch_add(1, std::memory_order_relaxed);
        g_standSamples.fetch_add(1, std::memory_order_relaxed);
    }
    // The root carries locomotion unsmoothed (smoothing its height made jumps and falls lag).
    const float rootZ = root[fresh][2];

    // Eye offset along the game camera's horizontal forward / right (camera-local Y forward, X right; world Z up).
    float fx = 2.0f * (q[0] * q[1] - q[3] * q[2]), fy = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
    float rx = 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]), ry = 2.0f * (q[0] * q[1] + q[3] * q[2]);
    const float fn = std::sqrt(fx * fx + fy * fy), rn = std::sqrt(rx * rx + ry * ry);
    if (fn > 1e-4f) { fx /= fn; fy /= fn; }
    if (rn > 1e-4f) { rx /= rn; ry /= rn; }
    const float ef = g_eyeFwd.load(std::memory_order_relaxed), eu = g_eyeUp.load(std::memory_order_relaxed);
    const float es = g_eyeSide.load(std::memory_order_relaxed);
    float e[3] = { root[fresh][0] + sm[0] + fx * ef + rx * es,
                   root[fresh][1] + sm[1] + fy * ef + ry * es,
                   rootZ + sm[2] + eu };
    {
        // Never behind her head (2026-10-13): the stabiliser (6 cm band, smoothing) let the camera lag behind or beside
        // the head when she stopped hard after running or turned on the spot -- it sat in her neck and saw her collar /
        // back. Against where the eye would be on the head itself: at most 2 cm behind along the view's horizontal
        // forward and 4 cm to either side; ahead is free.
        const float ix = head[fresh][0] + fx * ef + rx * es, iy = head[fresh][1] + fy * ef + ry * es;
        const float dx = e[0] - ix, dy = e[1] - iy;
        const float along = dx * fx + dy * fy, side = dx * rx + dy * ry;
        // Running, the near clip (up to nearRunCm) hides her neck and collar anyway: the allowances grow by as much, so the
        // stabiliser keeps the stride's sway out (pulled in 90 % of running frames before); slowing down shrinks them back.
        const float extra = run * (std::max)(0.0f, hs.nearRunCm - hs.nearCm);
        const float behind = 2.0f + extra, beside = 4.0f + extra, below = 3.0f + extra;
        float pullF = 0.0f, pullS = 0.0f;
        if (along < -behind) pullF = -behind - along;
        if (side > beside) pullS = beside - side; else if (side < -beside) pullS = -beside - side;
        // Nor more than 3 cm below it (2026-10-27): climbing a ladder her head rises ~1.2 m/s against the root and the
        // smoothing (150 ms with stabilisation) left the camera a median 18 cm below it, in her neck -- the collar came
        // in and out of view. Above is free, so the stabiliser still holds the camera up while her head bobs down.
        const float iz = head[fresh][2] + eu;
        const float pullU = e[2] < iz - below ? iz - below - e[2] : 0.0f;
        e[2] += pullU;
        if (pullF != 0.0f || pullS != 0.0f || pullU != 0.0f) {
            e[0] += fx * pullF + rx * pullS;
            e[1] += fy * pullF + ry * pullS;
            const uint32_t mm = static_cast<uint32_t>(std::sqrt(pullF * pullF + pullS * pullS + pullU * pullU) * 10.0f);
            uint32_t prev = g_stMaxPullMm.load(std::memory_order_relaxed);
            while (mm > prev && !g_stMaxPullMm.compare_exchange_weak(prev, mm, std::memory_order_relaxed)) {}
            g_stPulls.fetch_add(1, std::memory_order_relaxed);
        }
    }
#if VRMOD_DIAG
    {
        // Head camera log (development build, 2026-10-27, ladder climbing felt "two steps forward, one back"): per game
        // frame the skeleton root's height, the raw head's, the head's offset above the root as followed (stabiliser)
        // and smoothed, and the camera's -- to vrmod_headcam.csv, 15 minutes from the first first-person frame.
        static FILE* hf = nullptr;
        static bool hdone = false;
        static LONGLONG hstart = 0;
        if (!hdone) {
            if (!hf) {
                if (fopen_s(&hf, "vrmod_headcam.csv", "w") != 0 || !hf) hdone = true;
                else {
                    fprintf(hf, "t_s,root_z,head_z,off_z,follow_z,sm_z,cam_z,stab,speed_mps,root_x,root_y,head_x,head_y,cam_x,cam_y\n");
                    hstart = t.QuadPart;
                    modlog::Line("HEADLOG: writing the head camera's heights per game frame to vrmod_headcam.csv (15 minutes)");
                }
            }
            if (hf) {
                const double ts = static_cast<double>(t.QuadPart - hstart) / static_cast<double>(f.QuadPart);
                if (ts > 900.0) {
                    fclose(hf);
                    hf = nullptr;
                    hdone = true;
                    modlog::Line("HEADLOG: done");
                } else {
                    static uint32_t rows = 0;
                    fprintf(hf, "%.5f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", ts, rootZ, head[fresh][2], off[2],
                            stab ? follow[2] : off[2], sm[2], e[2], stab ? 1 : 0, g_speedMps.load(std::memory_order_relaxed), root[fresh][0],
                            root[fresh][1], head[fresh][0], head[fresh][1], e[0], e[1]);
                    if (++rows % 72 == 0) fflush(hf);
                }
            }
        }
    }
#endif
    {
        // Statistics: vertical travel of the unstabilised head vs the camera.
        const float rawZ = head[fresh][2];
        static float lastUsedZ = 0.0f;
        if (haveUsed) {
            g_stRawTravel.fetch_add(static_cast<uint64_t>(std::fabs(rawZ - lastRawZ) * 100.0f), std::memory_order_relaxed);
            g_stUsedTravel.fetch_add(static_cast<uint64_t>(std::fabs(e[2] - lastUsedZ) * 100.0f), std::memory_order_relaxed);
        }
        lastRawZ = rawZ;
        lastUsedZ = e[2];
    }
    if (haveUsed) {
        const float sx = e[0] - lastUsed[0], sy = e[1] - lastUsed[1], sz = e[2] - lastUsed[2];
        const float step = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (step > 150.0f) {   // no head moves 1.5 m in one frame: don't show this one...
            ++winJumps;
            for (int k = 0; k < 3; ++k) lastUsed[k] = e[k];   // ...but if she really was moved, the next frame agrees
            haveSm = false;                                     // and the smoothing restarts there
            endWindow();
            cachedOk = false;
            return false;
        }
        const uint32_t mm = static_cast<uint32_t>(step * 10.0f);
        uint32_t prev = g_stMaxStepMm.load(std::memory_order_relaxed);
        while (mm > prev && !g_stMaxStepMm.compare_exchange_weak(prev, mm, std::memory_order_relaxed)) {}
    }
    if (endWindow()) return false;
    haveUsed = true;
    for (int k = 0; k < 3; ++k) { out[k] = e[k]; cached[k] = e[k]; lastUsed[k] = e[k]; }
    cachedOk = true;
    g_lastEyeMs.store(NowMs(), std::memory_order_relaxed);
    return true;
}

float BoneScanNearOverrideCm() {
    return g_nearCm.load(std::memory_order_relaxed);
}

void BoneScanOnPresent() {
    // Ctrl+Alt+Z (the +0x10C near test) retired: writing it had no visible effect. It now drives near_probe.cpp.
    const bool start = KeyZ(false);
    HeadCamPresent();
    switch (g_phase) {
    case Idle: {
        if (!start) {
            // Head-bone camera: locate Lara's skeleton automatically while first person is on and it isn't known (or
            // was lost, e.g. after a level load), around the offset camera -- which sits just in front of her head.
            static double gameplaySinceMs = 0.0;
            if (MenuScreenActive() || gameplaySinceMs == 0.0) gameplaySinceMs = MenuScreenActive() ? 0.0 : NowMs();
            const bool settled = gameplaySinceMs > 0.0 && NowMs() - gameplaySinceMs > 1000.0;
            float face[3];
            const bool faceFresh = FaceHeadPoint(face, 500.0);
            // Face-guided tries repeat every 0.7 s (each costs a sliced memory sweep); offset-camera tries back off.
            const double every = faceFresh ? 700.0 : (g_locateFailures > 5 ? 5000.0 : 1000.0);
            if (g_headEnabled.load() && IsFirstPersonEnabled() && settled && (g_hintValid.load() || faceFresh)
                && (g_headBufA.load() == 0 || g_headLost.load())
                && NowMs() - g_lastLocateMs > every) {
                g_headLost.store(false);
                g_headBufA.store(0);
                g_headBufB.store(0);
                g_faceGuided = faceFresh;
                if (faceFresh) { Tx = face[0]; Ty = face[1]; Tz = face[2]; }
                else { Tx = g_hint[0].load(); Ty = g_hint[1].load(); Tz = g_hint[2].load(); }
                g_hits.clear();
                BuildRegions();
                g_locating = true;
                g_nearRadiusCm = faceFresh ? kFaceLocateCm : LocateRadiusCm();
                g_sliceMs = 4.0;   // gentler on the frame: a locate runs during play
                g_lastLocateMs = NowMs();
                g_phase = Scan;
            }
            return;
        }
        g_sliceMs = kSliceMs;
        g_nearRadiusCm = kNearCm;
        CamPose pose{};
        if (!CameraReadStructPose(&pose) || MenuScreenActive()) {
            modlog::Line("BONESCAN: start it in gameplay (camera not readable or a menu is open).");
            PlayCue(Cue::Failure);
            return;
        }
        Tx = pose.p[0]; Ty = pose.p[1]; Tz = pose.p[2];
        for (int k = 0; k < 4; ++k) Tq[k] = pose.q[k];
        g_hits.clear();
        g_hits.reserve(4096);
        BuildRegions();
        g_phase = Scan;
        modlog::Line("BONESCAN: scanning " + std::to_string(g_totalBytes >> 20) + " MB for bone matrices near the camera ("
            + std::to_string(Tx) + ", " + std::to_string(Ty) + ", " + std::to_string(Tz) + "). Stand still.");
        PlayCue(Cue::On);
        return;
    }
    case Scan:
        if (!Slice(ScanGuarded, "matrix scan")) return;
        g_scanMs = NowMs() - g_startMs;
        GrowCandidates();
        if (g_attachScanning) {
            g_attachScanning = false;
            FinishAttachScan();
            g_phase = Idle;
            return;
        }
        if (g_locating) {
            g_locating = false;
            ChoosePair();
            g_phase = Idle;
            return;
        }
        modlog::Line("BONESCAN: " + std::to_string(g_hits.size()) + " matrices near the camera, " + std::to_string(g_cands.size())
            + " arrays; searching for pointers to them.");
        BuildRegions();
        g_phase = Pointers;
        return;
    case Pointers:
        if (!Slice(PointersGuarded, "pointer scan")) return;
        g_ptrMs = NowMs() - g_startMs;
        g_sampleCount = 0;
        g_phase = Sample;
        return;
    case Sample:
        SampleOnce();
        if (++g_sampleCount < kSamplePresents) return;
        Report();
        g_phase = Idle;
        return;
    }
}

}
