#include "openvr_gaze.h"

#include <windows.h>
#include <openvr.h>   // Valve's OpenVR SDK header (BSD-3-Clause, third_party/openvr); only its types are used

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "logger.h"
#include "perf_stats.h"
#include "settings.h"

namespace vrmod {

namespace {

// openvr_api.dll exports, resolved at runtime (we don't link its import library).
using InitInternal2Fn = uint32_t(__cdecl*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
using GetGenericInterfaceFn = void*(__cdecl*)(const char*, vr::EVRInitError*);
using InitErrorTextFn = const char*(__cdecl*)(vr::EVRInitError);

vr::IVRSystem* g_system = nullptr;
bool g_givenUp = false;
double g_nextTry = 0.0;
int g_tries = 0;

std::mutex g_mutex;
float g_dir[2][3] = { { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, -1.0f } };
double g_sampleMs = 0.0;
bool g_have = false;
uint64_t g_polls = 0, g_valid = 0, g_stale = 0;
vr::HmdVector2_t g_lastNdc[2] = {};
// SteamVR keeps reporting the last gaze as valid when the eye tracker stops (seen with the Pimax/Tobii runtime losing
// the tracker, and with the headset taken off). A live tracker's samples never repeat exactly, so a gaze that hasn't
// changed for kStaleMs is treated as lost; shorter pauses (blinks) keep the last gaze.
constexpr double kStaleMs = 1000.0;
uint64_t g_stamp = 0;   // advances with every new (changed) gaze sample
vr::HmdVector2_t g_prevNdc[2] = {};
double g_changedMs = 0.0;
bool g_staleLogged = false;

// The runtime folder SteamVR registered: the first entry of "runtime" in openvrpaths.vrpath (JSON, backslashes escaped).
std::wstring RuntimeFolder() {
    wchar_t local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    std::ifstream f(std::wstring(local) + L"\\openvr\\openvrpaths.vrpath");
    if (!f) return L"";
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    size_t k = s.find("\"runtime\"");
    if (k == std::string::npos || (k = s.find('[', k)) == std::string::npos || (k = s.find('"', k)) == std::string::npos) return L"";
    std::string path;
    for (size_t i = k + 1; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        path += s[i];
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    return w;
}

bool Connect() {
    HMODULE api = GetModuleHandleW(L"openvr_api.dll");
    if (!api) {
        const std::wstring folder = RuntimeFolder();
        if (folder.empty()) {
            modlog::Line("OpenVR gaze: no SteamVR runtime registered (openvrpaths.vrpath); off.");
            g_givenUp = true;
            return false;
        }
        api = LoadLibraryExW((folder + L"\\bin\\win64\\openvr_api.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!api) {
            modlog::Line("OpenVR gaze: SteamVR's openvr_api.dll didn't load (error " + std::to_string(GetLastError()) + "); off.");
            g_givenUp = true;
            return false;
        }
    }
    const auto init = reinterpret_cast<InitInternal2Fn>(GetProcAddress(api, "VR_InitInternal2"));
    const auto get = reinterpret_cast<GetGenericInterfaceFn>(GetProcAddress(api, "VR_GetGenericInterface"));
    const auto text = reinterpret_cast<InitErrorTextFn>(GetProcAddress(api, "VR_GetVRInitErrorAsEnglishDescription"));
    if (!init || !get) {
        modlog::Line("OpenVR gaze: openvr_api.dll lacks VR_InitInternal2 / VR_GetGenericInterface; off.");
        g_givenUp = true;
        return false;
    }
    vr::EVRInitError err = vr::VRInitError_None;
    init(&err, vr::VRApplication_Background, nullptr);
    if (err == vr::VRInitError_None) g_system = static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version, &err));
    if (err != vr::VRInitError_None || !g_system) {
        g_system = nullptr;
        modlog::Line(std::string("OpenVR gaze: connecting failed (") + std::to_string(static_cast<int>(err)) + " " +
                     (text ? text(err) : "") + ")" + (g_tries >= 5 ? "; giving up." : "; retrying in 10 s."));
        if (g_tries >= 5) g_givenUp = true;
        return false;
    }
    const char* rt = g_system->GetRuntimeVersion();
    char b[256];
    snprintf(b, sizeof(b), "OpenVR gaze: connected to SteamVR %s as a background application (%s).", rt ? rt : "?", vr::IVRSystem_Version);
    modlog::Line(b);
    return true;
}

// One eye's NDC foveation centre (relative to that eye's OpenVR projection) as a head-relative direction.
bool NdcToHeadDirection(vr::EVREye eye, const vr::HmdVector2_t& ndc, float out[3]) {
    if (!std::isfinite(ndc.v[0]) || !std::isfinite(ndc.v[1])) return false;
    float l = 0, r = 0, t = 0, b = 0;   // tangents; top/bottom down-positive
    g_system->GetProjectionRaw(eye, &l, &r, &t, &b);
    if (!(r > l) || !(b > t)) return false;
    const float tx = l + (ndc.v[0] + 1.0f) * 0.5f * (r - l);
    const float tyDown = t + (1.0f - ndc.v[1]) * 0.5f * (b - t);
    const float d[3] = { tx, -tyDown, -1.0f };   // eye space: x right, y up, -z forward
    const vr::HmdMatrix34_t m = g_system->GetEyeToHeadTransform(eye);   // rotation part: eye basis in head space
    float h[3];
    for (int i = 0; i < 3; ++i) h[i] = m.m[i][0] * d[0] + m.m[i][1] * d[1] + m.m[i][2] * d[2];
    const float len = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    if (!(len > 1e-6f)) return false;
    for (int i = 0; i < 3; ++i) out[i] = h[i] / len;
    return true;
}

} // namespace

void OpenVrGazePoll() {
    if (g_givenUp) return;
    if (!g_system) {
        if (NowMs() < g_nextTry) return;
        if (!GetGazeOpenVr()) {
            modlog::Line("OpenVR gaze: off ([gaze] openvr=0).");
            g_givenUp = true;
            return;
        }
        ++g_tries;
        g_nextTry = NowMs() + 10000.0;
        if (!Connect()) return;
    }
    vr::HmdVector2_t ndc[2] = {};
    const bool ok = g_system->GetEyeTrackedFoveationCenter(&ndc[0], &ndc[1]);
    const double now = NowMs();
    bool changed = false;
    if (!ok || memcmp(ndc, g_prevNdc, sizeof ndc) != 0) {
        memcpy(g_prevNdc, ndc, sizeof ndc);
        g_changedMs = now;
        changed = ok;
    }
    const bool stale = ok && now - g_changedMs > kStaleMs;
    if (stale != g_staleLogged) {
        g_staleLogged = stale;
        modlog::Line(stale ? "OpenVR gaze: the gaze stopped changing -- eye tracker lost (headset off, or the Pimax/Tobii eye "
                             "tracker disconnected); foveation falls back to the centre."
                           : "OpenVR gaze: the gaze is live again.");
    }
    float dir[2][3];
    const bool valid = ok && !stale && NdcToHeadDirection(vr::Eye_Left, ndc[0], dir[0]) && NdcToHeadDirection(vr::Eye_Right, ndc[1], dir[1]);
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_polls;
    if (stale) ++g_stale;
    if (valid) {
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < 3; ++k) g_dir[e][k] = dir[e][k];
        g_lastNdc[0] = ndc[0];
        g_lastNdc[1] = ndc[1];
        g_sampleMs = NowMs();
        g_have = true;
        if (changed) ++g_stamp;
        ++g_valid;
    }
    static double lastDiag = 0.0;
    if (NowMs() - lastDiag > 10000.0) {
        lastDiag = NowMs();
        char b[320];
        snprintf(b, sizeof(b), "OpenVR gaze (10 s): %llu polls, %llu valid, %llu stale | last NDC L (%.3f, %.3f) R (%.3f, %.3f) | "
                 "direction L (%.3f, %.3f, %.3f) R (%.3f, %.3f, %.3f)",
                 static_cast<unsigned long long>(g_polls), static_cast<unsigned long long>(g_valid),
                 static_cast<unsigned long long>(g_stale), g_lastNdc[0].v[0], g_lastNdc[0].v[1], g_lastNdc[1].v[0], g_lastNdc[1].v[1],
                 g_dir[0][0], g_dir[0][1], g_dir[0][2], g_dir[1][0], g_dir[1][1], g_dir[1][2]);
        modlog::Line(b);
        g_polls = g_valid = g_stale = 0;
    }
}

bool OpenVrGazeLatest(int eye, float dir[3], double maxAgeMs, uint64_t* stamp) {
    if (eye < 0 || eye > 1) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_have || NowMs() - g_sampleMs > maxAgeMs) return false;
    for (int k = 0; k < 3; ++k) dir[k] = g_dir[eye][k];
    if (stamp) *stamp = g_stamp;
    return true;
}

}
