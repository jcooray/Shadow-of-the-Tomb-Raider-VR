#include "aim_finder.h"

#include <windows.h>
#include <algorithm>
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
#include "logger.h"
#include "perf_stats.h"

namespace vrmod {

namespace {

// A0 (docs/AIM.md): which values in memory are the game camera controller's yaw and pitch?
// Ctrl+Alt+Y: walk pointers (depth <= kDepth, <= kMaxObjects objects of kObjBytes each) from four roots --
//   the gameplay camera object (RCX at the camera read hook), the camera manager block around our struct, and the two
//   camera globals the manager's update uses (DAT_1416bb1f0, DAT_1436aba00) --
// then take a snapshot of all those objects each time the game camera has settled at a new angle (>= kMinStepDeg from
// every earlier snapshot), up to kSnapshots. Report every float whose value tracks the game camera's own yaw or pitch
// (value = a * angle + b, a = +-1 in radians or degrees, b constant, allowing a full-turn wrap) or its forward
// direction's components, with the pointer path from its root.
constexpr int kDepth = 4;
constexpr int kMaxObjects = 6000;
constexpr size_t kObjBytes = 0x1000;
constexpr int kSnapshots = 6;
constexpr float kMinStepDeg = 5.0f;
constexpr int kStillFrames = 30;
constexpr double kTimeoutMs = 90000.0;

struct Node { uintptr_t addr; int parent; int offInParent; int depth; int root; };
struct Snap { float yaw, pitch, fx, fy, fz; float q[4]; std::vector<uint8_t> bytes; };   // bytes: all objects, kObjBytes each

const char* const kRootNames[] = { "gameplayCamera(RCX)", "cameraManagerBlock", "DAT_1416bb1f0", "DAT_1436aba00",
                                   "cameraComponent", "instanceComponent", "slot0Object(node+0x520)" };

std::vector<Node> g_nodes;
// A0b: yaw-angle matches (distinct addresses) kept for the write test.
struct Match { uintptr_t addr; int node; size_t off; float sign, unit; std::string what; int kind = 0; };
std::vector<Match> g_yawMatches;
enum class TestState { None, Ready, Running };
TestState g_test = TestState::None;
size_t g_testIdx = 0;
int g_testPhase = 0, g_testFrames = 0;
float g_testYaw0 = 0.0f, g_testV0 = 0.0f;
std::ostringstream g_testLog;
std::vector<Snap> g_snaps;
bool g_active = false;
double g_startMs = 0.0;
int g_still = 0;
float g_lastYaw = 0.0f, g_lastPitch = 0.0f;

bool ReadGuarded(uintptr_t a, void* out, size_t n) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Readable heap regions (committed, read/write, not image, not GPU-mapped), sorted, built once per walk: a pointer
// check is then a binary search instead of a VirtualQuery per candidate.
std::vector<std::pair<uintptr_t, uintptr_t>> g_regions;

void BuildRegions() {
    g_regions.clear();
    uintptr_t p = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (p < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), next = base + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool rd = prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && rd && !(mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) && mbi.Type != MEM_IMAGE)
            g_regions.push_back({ base, next });
        if (next <= p) break;
        p = next;
    }
}

bool Readable(uintptr_t a) {
    if (a < 0x10000 || a > 0x7FFFFFFFFFFFull || (a & 7)) return false;
    auto it = std::upper_bound(g_regions.begin(), g_regions.end(), std::make_pair(a, ~uintptr_t(0)));
    if (it == g_regions.begin()) return false;
    --it;
    return a >= it->first && a + 8 <= it->second;
}

void Angles(float* yaw, float* pitch, float* fx, float* fy, float* fz) {
    float q[4];
    CameraGameQuat(q);
    // Camera-local forward is +Y; world Z is up.
    const float x = 2.0f * (q[0] * q[1] - q[3] * q[2]);
    const float y = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
    const float z = 2.0f * (q[1] * q[2] + q[3] * q[0]);
    *fx = x; *fy = y; *fz = z;
    *yaw = std::atan2(x, y);
    *pitch = std::asin((std::max)(-1.0f, (std::min)(1.0f, z)));
}

void Walk() {
    g_nodes.clear();
    BuildRegions();
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t s = CameraStructAddress();
    uintptr_t roots[4] = { CameraOwnerAddress(), s ? s - 0x2B0 : 0, 0, 0 };
    ReadGuarded(exe + 0x16BB1F0, &roots[2], 8);
    ReadGuarded(exe + 0x36ABA00, &roots[3], 8);
    // The camera director (DAT_1416bb1f0) keeps a hash map of camera slots: buckets at +0x68, bucket count at +0x80,
    // nodes chained through +0x0 with the slot key at +0x10 (FUN_140170cf0 / FUN_140170bd0). A node's +0x1D0 is the
    // slot's finished transform (the yaw copies the first run found); the active camera object behind the slot is
    // *(*(node + 0x520) + 0x20) (FUN_140170bd0 -> FUN_140188dc0) -- one level beyond the first run's depth 3.
    uintptr_t slotObj = 0, slotCam = 0;
    if (roots[2]) {
        uintptr_t buckets = 0, count = 0;
        if (ReadGuarded(roots[2] + 0x68, &buckets, 8) && ReadGuarded(roots[2] + 0x80, &count, 8) && buckets && count) {
            uintptr_t node = 0;
            ReadGuarded(buckets, &node, 8);   // key 0 lands in bucket 0 % count = 0
            for (int guard = 0; node && guard < 64; ++guard) {
                uint32_t key = ~0u;
                if (!ReadGuarded(node + 0x10, &key, 4)) break;
                if (key == 0) {
                    ReadGuarded(node + 0x520, &slotObj, 8);
                    if (slotObj) ReadGuarded(slotObj + 0x20, &slotCam, 8);
                    break;
                }
                if (!ReadGuarded(node, &node, 8)) break;
            }
        }
    }
    {
        char b[160];
        snprintf(b, sizeof(b), "AIMFIND: director 0x%llX, slot-0 object 0x%llX, slot-0 camera 0x%llX, gameplay camera (RCX) 0x%llX",
                 static_cast<unsigned long long>(roots[2]), static_cast<unsigned long long>(slotObj), static_cast<unsigned long long>(slotCam),
                 static_cast<unsigned long long>(roots[0]));
        modlog::Line(b);
    }
    // The "+0x20" value is an INSTANCE HANDLE (FUN_140188dc0): index = low 32 bits (< 20000) into the global instance
    // table (0x14301D0C0, 0x120 per entry); the entry's +0x110 (0x14301D1D0) must equal the handle; +0xD8 (0x14301D198)
    // points to its component array, +0xD0 (0x14301D190) is the count; the component at index *(u32*)0x1416B99D4 is
    // the camera component the director resolves. All components of that instance become roots (camera first).
    std::vector<uintptr_t> comps;
    uint32_t camIdx = ~0u;
    const uint64_t handle = slotCam;   // what the first A0c build took for a pointer
    if ((handle >> 32) != 0 && static_cast<uint32_t>(handle) < 20000) {
        const uintptr_t e = exe + 0x301D0C0 + static_cast<uintptr_t>(static_cast<uint32_t>(handle)) * 0x120;
        uint64_t check = 0;
        uint32_t count = 0;
        uintptr_t arr = 0;
        if (ReadGuarded(e + 0x110, &check, 8) && check == handle && ReadGuarded(e + 0xD0, &count, 4) && ReadGuarded(e + 0xD8, &arr, 8)
            && arr && count > 0 && count < 256) {
            ReadGuarded(exe + 0x16B99D4, &camIdx, 4);
            for (uint32_t c = 0; c < count; ++c) {
                uintptr_t comp = 0;
                ReadGuarded(arr + c * 8, &comp, 8);
                comps.push_back(comp);
            }
        }
    }
    {
        char b[200];
        snprintf(b, sizeof(b), "AIMFIND: instance handle 0x%llX: %zu components, camera component index %u -> 0x%llX",
                 static_cast<unsigned long long>(handle), comps.size(), camIdx,
                 static_cast<unsigned long long>(camIdx < comps.size() ? comps[camIdx] : 0));
        modlog::Line(b);
    }
    std::unordered_map<uintptr_t, int> seen;
    auto addRoot = [&](uintptr_t a, int r) {
        if (!a || seen.count(a) || !Readable(a)) return;
        seen[a] = static_cast<int>(g_nodes.size());
        g_nodes.push_back({ a, -1, 0, 0, r });
    };
    if (camIdx < comps.size()) addRoot(comps[camIdx], 4);   // the camera component first, so its subtree gets the budget
    for (uintptr_t c : comps) addRoot(c, 5);
    addRoot(slotObj, 6);
    for (int r = 0; r < 4; ++r) {
        if (!roots[r] || seen.count(roots[r])) continue;
        seen[roots[r]] = static_cast<int>(g_nodes.size());
        g_nodes.push_back({ roots[r], -1, 0, 0, r });
    }
    std::vector<uint8_t> buf(kObjBytes);
    for (size_t i = 0; i < g_nodes.size() && g_nodes.size() < static_cast<size_t>(kMaxObjects); ++i) {
        const Node n = g_nodes[i];
        if (n.depth >= kDepth) continue;
        if (!ReadGuarded(n.addr, buf.data(), kObjBytes)) continue;
        for (size_t off = 0; off + 8 <= kObjBytes; off += 8) {
            uintptr_t p;
            memcpy(&p, buf.data() + off, 8);
            if (seen.count(p) || !Readable(p)) continue;
            seen[p] = static_cast<int>(g_nodes.size());
            g_nodes.push_back({ p, static_cast<int>(i), static_cast<int>(off), n.depth + 1, n.root });
            if (g_nodes.size() >= static_cast<size_t>(kMaxObjects)) break;
        }
    }
}

void TakeSnapshot(float yaw, float pitch, float fx, float fy, float fz) {
    Snap sn{ yaw, pitch, fx, fy, fz, { 0, 0, 0, 1 }, std::vector<uint8_t>(g_nodes.size() * kObjBytes, 0) };
    CameraGameQuat(sn.q);
    for (size_t i = 0; i < g_nodes.size(); ++i) ReadGuarded(g_nodes[i].addr, sn.bytes.data() + i * kObjBytes, kObjBytes);
    g_snaps.push_back(std::move(sn));
}

std::string Path(int i) {
    std::string s;
    while (i >= 0) {
        const Node& n = g_nodes[i];
        char b[64];
        if (n.parent < 0) snprintf(b, sizeof(b), "%s", kRootNames[n.root]);
        else snprintf(b, sizeof(b), " ->[+0x%X]", n.offInParent);
        s = b + s;
        i = n.parent;
    }
    return s;
}

std::string Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

// v tracks a*angle + b (a = sign * unit), allowing a full-turn wrap for yaw.
bool Tracks(const std::vector<float>& v, const std::vector<float>& ang, float sign, float unit, bool wrap, float tol, float* offset) {
    const float turn = 6.2831853f * unit;
    const float b0 = v[0] - sign * unit * ang[0];
    for (size_t s = 1; s < v.size(); ++s) {
        float d = (v[s] - sign * unit * ang[s]) - b0;
        if (wrap) d -= turn * std::round(d / turn);
        if (std::fabs(d) > tol) return false;
    }
    *offset = b0;
    return true;
}

void Report() {
    const size_t nf = kObjBytes / 4;
    std::vector<float> yaw, pitch, fx, fy, fz;
    for (const Snap& s : g_snaps) { yaw.push_back(s.yaw); pitch.push_back(s.pitch); fx.push_back(s.fx); fy.push_back(s.fy); fz.push_back(s.fz); }
    auto range = [](const std::vector<float>& a) { return *std::max_element(a.begin(), a.end()) - *std::min_element(a.begin(), a.end()); };
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(4);
    o << "Aim finder " << Stamp() << ": " << g_nodes.size() << " objects (" << kObjBytes << " bytes each, depth <= " << kDepth << "), "
      << g_snaps.size() << " snapshots\n";
    for (size_t s = 0; s < g_snaps.size(); ++s)
        o << "  snapshot " << s << ": game camera yaw " << yaw[s] * 57.29578f << " deg, pitch " << pitch[s] * 57.29578f << " deg\n";
    o << "yaw range " << range(yaw) * 57.29578f << " deg, pitch range " << range(pitch) * 57.29578f << " deg\n\n";
    int hits = 0;
    for (size_t i = 0; i < g_nodes.size(); ++i) {
        for (size_t k = 0; k < nf; ++k) {
            std::vector<float> v;
            bool finite = true;
            for (const Snap& s : g_snaps) {
                float f;
                memcpy(&f, s.bytes.data() + i * kObjBytes + k * 4, 4);
                if (!(f == f) || std::fabs(f) > 1e6f) { finite = false; break; }
                v.push_back(f);
            }
            if (!finite || range(v) < 1e-4f) continue;
            struct T { const char* what; const std::vector<float>* ang; float sign, unit; bool wrap; float tol; bool need; };
            const T tests[] = {
                { "yaw rad", &yaw, 1, 1, true, 0.01f, range(yaw) > 0.3f }, { "-yaw rad", &yaw, -1, 1, true, 0.01f, range(yaw) > 0.3f },
                { "yaw deg", &yaw, 1, 57.29578f, true, 0.5f, range(yaw) > 0.3f }, { "-yaw deg", &yaw, -1, 57.29578f, true, 0.5f, range(yaw) > 0.3f },
                { "pitch rad", &pitch, 1, 1, false, 0.01f, range(pitch) > 0.15f }, { "-pitch rad", &pitch, -1, 1, false, 0.01f, range(pitch) > 0.15f },
                { "pitch deg", &pitch, 1, 57.29578f, false, 0.5f, range(pitch) > 0.15f }, { "-pitch deg", &pitch, -1, 57.29578f, false, 0.5f, range(pitch) > 0.15f },
                { "forward x", &fx, 1, 1, false, 0.01f, range(fx) > 0.1f }, { "-forward x", &fx, -1, 1, false, 0.01f, range(fx) > 0.1f },
                { "forward y", &fy, 1, 1, false, 0.01f, range(fy) > 0.1f }, { "-forward y", &fy, -1, 1, false, 0.01f, range(fy) > 0.1f },
                { "forward z", &fz, 1, 1, false, 0.01f, range(fz) > 0.1f }, { "-forward z", &fz, -1, 1, false, 0.01f, range(fz) > 0.1f },
            };
            for (const T& t : tests) {
                float off = 0.0f;
                if (!t.need || !Tracks(v, *t.ang, t.sign, t.unit, t.wrap, t.tol, &off)) continue;
                if (++hits > 400) break;
                if (std::strstr(t.what, "yaw") && (t.unit == 1.0f || t.unit > 50.0f)) {
                    const uintptr_t a = g_nodes[i].addr + k * 4;
                    bool dup = false;
                    for (const Match& m : g_yawMatches) if (m.addr == a) dup = true;
                    if (!dup) g_yawMatches.push_back({ a, static_cast<int>(i), k * 4, t.sign, t.unit, t.what });
                }
                char b[96];
                snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(g_nodes[i].addr + k * 4));
                o << b << "  " << t.what << " + " << off << "  | " << Path(static_cast<int>(i)) << " +0x" << std::hex << k * 4 << std::dec << "  | values";
                for (float f : v) o << ' ' << f;
                o << "\n";
            }
        }
    }
    // A0d: contiguous forward vectors (x, y, z at +0/+4/+8, same sign) and quaternions (x, y, z, w tracking the game
    // camera's quaternion, either sign) -- a controller may keep its orientation that way.
    for (size_t i = 0; i < g_nodes.size(); ++i) {
        for (size_t k = 0; k + 4 <= nf; ++k) {
            auto val = [&](size_t s2, size_t kk) { float f; memcpy(&f, g_snaps[s2].bytes.data() + i * kObjBytes + kk * 4, 4); return f; };
            for (float sg : { 1.0f, -1.0f }) {
                bool vec = k + 3 <= nf, quat = true;
                for (size_t s2 = 0; s2 < g_snaps.size(); ++s2) {
                    const Snap& sn = g_snaps[s2];
                    auto within = [](float a, float b) { return std::fabs(a - b) <= 0.01f; };   // false for NaN
                    if (vec && !(within(val(s2, k), sg * sn.fx) && within(val(s2, k + 1), sg * sn.fy) && within(val(s2, k + 2), sg * sn.fz))) vec = false;
                    for (int c = 0; c < 4 && quat; ++c) if (!within(val(s2, k + c), sg * sn.q[c])) quat = false;
                }
                const uintptr_t a = g_nodes[i].addr + k * 4;
                bool dup = false;
                for (const Match& m : g_yawMatches) if (m.addr == a) dup = true;
                if (dup) continue;
                if (vec) {
                    g_yawMatches.push_back({ a, static_cast<int>(i), k * 4, sg, 1.0f, sg > 0 ? "forward vec3" : "-forward vec3", 1 });
                    o << "VEC3 0x" << std::hex << a << std::dec << " " << (sg > 0 ? "forward" : "-forward") << " | " << Path(static_cast<int>(i)) << " +0x" << std::hex << k * 4 << std::dec << "\n";
                } else if (quat) {
                    g_yawMatches.push_back({ a, static_cast<int>(i), k * 4, sg, 1.0f, sg > 0 ? "quaternion" : "-quaternion", 2 });
                    o << "QUAT 0x" << std::hex << a << std::dec << " " << (sg > 0 ? "q" : "-q") << " | " << Path(static_cast<int>(i)) << " +0x" << std::hex << k * 4 << std::dec << "\n";
                }
            }
        }
    }
    std::stable_sort(g_yawMatches.begin(), g_yawMatches.end(), [](const Match& a, const Match& b) {
        auto rank = [](int kind) { return kind == 2 ? 0 : kind == 0 ? 1 : 2; };
        return rank(a.kind) < rank(b.kind);
    });
    constexpr size_t kMaxTest = 60;
    if (g_yawMatches.size() > kMaxTest) {
        o << "\n(" << g_yawMatches.size() << " candidates; only the first " << kMaxTest << " are write-tested)";
        g_yawMatches.resize(kMaxTest);
    }
    o << "\n" << hits << " matches; " << g_yawMatches.size() << " distinct yaw-angle addresses kept for the write test "
      << "(Ctrl+Alt+Y again).\n";
    const std::string name = "vrmod_aimfind_" + Stamp() + ".txt";
    std::ofstream(name, std::ios::binary) << o.str();
    modlog::Line("AIMFIND: " + std::to_string(hits) + " values track the game camera's yaw/pitch -> " + name);
    PlayCue(hits > 0 ? Cue::Success : Cue::Failure);
    g_test = g_yawMatches.empty() ? TestState::None : TestState::Ready;
}

float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

void WriteGuarded(uintptr_t a, float v) {
    __try { *reinterpret_cast<volatile float*>(a) = v; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// A0b: for each yaw-angle address -- wait until the camera is still, add 15 degrees (in the value's own unit and
// sign), and measure the game camera's yaw 15 and 75 frames later. A copy is simply overwritten (no turn); the
// controller's state turns the camera and the turn persists.
void TestStep(float yaw) {
    if (g_testIdx >= g_yawMatches.size()) {
        g_test = TestState::None;
        g_testLog << "done.\n";
        const std::string name = "vrmod_aimtest_" + Stamp() + ".txt";
        std::ofstream(name, std::ios::binary) << g_testLog.str();
        modlog::Line("AIMTEST: finished -> " + name);
        PlayCue(Cue::Success);
        return;
    }
    const Match& m = g_yawMatches[g_testIdx];
    ++g_testFrames;
    if (g_testPhase == 0) {                       // wait for a still camera
        if (g_still >= 10) {
            g_testYaw0 = yaw;
            float v = 0.0f;
            if (!ReadGuarded(m.addr, &v, 4)) { ++g_testIdx; g_testFrames = 0; return; }
            g_testV0 = v;
            const float d = 0.2618f;   // 15 degrees
            if (m.kind == 0) {
                WriteGuarded(m.addr, v + m.sign * m.unit * d);   // +15 degrees of camera yaw, if it is the source
            } else if (m.kind == 1) {
                float x = 0, y = 0;
                ReadGuarded(m.addr, &x, 4);
                ReadGuarded(m.addr + 4, &y, 4);
                // yaw = atan2(fx, fy): yaw + d gives fx' = fx cos d + fy sin d, fy' = -fx sin d + fy cos d.
                WriteGuarded(m.addr, x * std::cos(d) + y * std::sin(d));
                WriteGuarded(m.addr + 4, -x * std::sin(d) + y * std::cos(d));
            } else {
                float q[4];
                for (int c = 0; c < 4; ++c) ReadGuarded(m.addr + c * 4, &q[c], 4);
                // r = rotation about world Z by -d (turns atan2(fx, fy) by +d); q' = r * q.
                const float rz = std::sin(-d * 0.5f), rw = std::cos(-d * 0.5f);
                const float nq[4] = { rw * q[0] - rz * q[1], rw * q[1] + rz * q[0], rw * q[2] + rz * q[3], rw * q[3] - rz * q[2] };
                for (int c = 0; c < 4; ++c) WriteGuarded(m.addr + c * 4, nq[c]);
            }
            g_testPhase = 1;
            g_testFrames = 0;
        } else if (g_testFrames > 600) {
            g_testLog << "candidate " << g_testIdx + 1 << ": camera never still, skipped\n";
            ++g_testIdx; g_testFrames = 0;
        }
    } else if (g_testPhase == 1 && g_testFrames == 15) {
        g_testPhase = 2;
        g_testLog.setf(std::ios::fixed);
        g_testLog.precision(2);
        g_testLog << "candidate " << g_testIdx + 1 << " (" << m.what << ", " << Path(m.node) << " +0x" << std::hex << m.off << std::dec
                  << "): after 15 frames the camera yaw moved " << WrapPi(yaw - g_testYaw0) * 57.29578f << " deg";
    } else if (g_testPhase == 2 && g_testFrames == 75) {
        g_testLog << ", after 75 frames " << WrapPi(yaw - g_testYaw0) * 57.29578f << " deg\n";
        modlog::Line("AIMTEST: candidate " + std::to_string(g_testIdx + 1) + "/" + std::to_string(g_yawMatches.size()) + " tested");
        PlayCue(Cue::On);
        ++g_testIdx;
        g_testPhase = 0;
        g_testFrames = 0;
    }
}

} // namespace

std::vector<AimWatchTarget> AimFinderWatchTargets() {
    std::vector<const Match*> order;
    for (int kind : { 0, 2, 1 })
        for (const Match& m : g_yawMatches) if (m.kind == kind) order.push_back(&m);
    std::vector<AimWatchTarget> out;
    std::vector<int> roots, nodes;
    std::vector<uintptr_t> addrs;
    for (int pass = 0; pass < 3 && out.size() < 4; ++pass) {   // one per root, then per object, then any
        for (const Match* m : order) {
            if (out.size() >= 4) break;
            const int root = g_nodes[m->node].root;
            if (std::find(addrs.begin(), addrs.end(), m->addr) != addrs.end()) continue;
            if (pass < 2 && std::find(nodes.begin(), nodes.end(), m->node) != nodes.end()) continue;
            if (pass == 0 && std::find(roots.begin(), roots.end(), root) != roots.end()) continue;
            roots.push_back(root);
            nodes.push_back(m->node);
            addrs.push_back(m->addr);
            std::ostringstream o;
            o << m->what << ", " << Path(m->node) << " +0x" << std::hex << m->off;
            out.push_back({ m->addr, o.str() });
        }
    }
    return out;
}

#ifdef AIM_FINDER_HARNESS
bool g_forceStart = false;
void AimFinderForceStart() { g_forceStart = true; }
bool AimFinderActive() { return g_active; }
#endif

void AimFinderOnPresent() {
    static bool yWas = false;
    bool y = (GetAsyncKeyState('Y') & 0x8000) != 0;
    bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
#ifdef AIM_FINDER_HARNESS
    if (g_forceStart) { g_forceStart = false; y = ca = true; yWas = false; }
#endif
    if (y && !yWas && ca && !g_active && g_test == TestState::Ready) {
        g_test = TestState::Running;
        g_testIdx = 0;
        g_testPhase = 0;
        g_testFrames = 0;
        g_testLog.str("");
        g_testLog << "Aim write test " << Stamp() << ": " << g_yawMatches.size() << " yaw-angle addresses; each gets +15 deg of yaw.\n";
        modlog::Line("AIMTEST: testing " + std::to_string(g_yawMatches.size()) + " addresses -- keep the camera still (mouse and stick untouched).");
        PlayCue(Cue::On);
        yWas = y;
        return;
    }
    if (g_test == TestState::Running && y && !yWas && ca) {
        yWas = y;
        g_test = TestState::None;
        g_testLog << "aborted by the user at candidate " << g_testIdx + 1 << ".\n";
        const std::string name = "vrmod_aimtest_" + Stamp() + ".txt";
        std::ofstream(name, std::ios::binary) << g_testLog.str();
        modlog::Line("AIMTEST: aborted -> " + name);
        PlayCue(Cue::Off);
        return;
    }
    if (g_test == TestState::Running) {
        yWas = y;
        float yaw, pitch, fx, fy, fz;
        Angles(&yaw, &pitch, &fx, &fy, &fz);
        const float dy = std::fabs(yaw - g_lastYaw), dp = std::fabs(pitch - g_lastPitch);
        g_lastYaw = yaw;
        g_lastPitch = pitch;
        g_still = (dy < 0.0009f && dp < 0.0009f) ? g_still + 1 : 0;
        TestStep(yaw);
        return;
    }
    if (y && !yWas && ca && !g_active) {
        g_yawMatches.clear();
        g_test = TestState::None;
        if (!CameraStructAddress() || !CameraOwnerAddress()) {
            modlog::Line("AIMFIND: camera not known yet (play for a moment first).");
            PlayCue(Cue::Failure);
        } else {
            const double t0 = NowMs();
            Walk();
            g_snaps.clear();
            g_active = true;
            g_startMs = NowMs();
            g_still = 0;
            modlog::Line("AIMFIND: watching " + std::to_string(g_nodes.size()) + " objects (walk " + std::to_string(static_cast<int>(NowMs() - t0))
                + " ms). Turn the camera with the mouse and pause after each turn: left, right, up, down, ... (" + std::to_string(kSnapshots) + " snapshots).");
            PlayCue(Cue::On);
        }
    }
    yWas = y;
    if (!g_active) return;
    float yaw, pitch, fx, fy, fz;
    Angles(&yaw, &pitch, &fx, &fy, &fz);
    const float dy = std::fabs(yaw - g_lastYaw), dp = std::fabs(pitch - g_lastPitch);
    g_lastYaw = yaw;
    g_lastPitch = pitch;
    g_still = (dy < 0.0009f && dp < 0.0009f) ? g_still + 1 : 0;   // < 0.05 degrees per frame
    if (g_still == kStillFrames) {
        bool fresh = true;
        for (const Snap& s : g_snaps) {
            float d = std::fabs(yaw - s.yaw);
            d = (std::min)(d, 6.2831853f - d);
            if (d < kMinStepDeg / 57.29578f && std::fabs(pitch - s.pitch) < kMinStepDeg / 57.29578f) fresh = false;
        }
        if (fresh) {
            TakeSnapshot(yaw, pitch, fx, fy, fz);
            char b[128];
            snprintf(b, sizeof(b), "AIMFIND: snapshot %zu/%d at yaw %.1f, pitch %.1f deg", g_snaps.size(), kSnapshots, yaw * 57.29578f, pitch * 57.29578f);
            modlog::Line(b);
            PlayCue(Cue::On);
        }
    }
    if (static_cast<int>(g_snaps.size()) >= kSnapshots || NowMs() - g_startMs > kTimeoutMs) {
        g_active = false;
        if (g_snaps.size() >= 3) Report();
        else { modlog::Line("AIMFIND: too few snapshots (" + std::to_string(g_snaps.size()) + "); try again."); PlayCue(Cue::Failure); }
    }
}

}
