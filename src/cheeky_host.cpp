// Gaze provider for CheekyFoveatedDLSS (foveated DLSS; our fork D:\Projects\CheekyFoveatedDLSS, branch
// sottr-vrmod-host; the interface is Cheeky's shared/cheeky_gaze_provider.h, docs/gaze-provider.md, offered upstream).
// Two exports from our dxgi.dll, found and called by Cheeky inside the game process:
//
//   CheekyGazeProvider_GetGazeSnapshot -- the gaze, in the layout of Cheeky's gaze snapshot (ABI version 4), which its own
//     OpenXR layer normally supplies: one view per eye, the gaze point in that eye's rendered image (u right, v down,
//     0..1), the eye's rendered FOV, and the forward point. Under AER each eye's image is rendered symmetrically around
//     the head's forward direction with the game's final projection, so a head-relative gaze direction maps straight to
//     an image point. The gaze comes from SteamVR's OpenVR interface per eye (openvr_gaze.cpp), else from our OpenXR
//     session's eye gaze action (one direction for both eyes).
//   CheekyGazeProvider_GetEyeViews -- which game DLSS feature handle is which eye (D1: slot 0 left, slot 1 right) and the
//     DLSS output size, so Cheeky can treat them as a calibrated stereo pair (full-image mapping).
//
// The structs below are written from Cheeky's documented interfaces (shared/cheeky_gaze_abi.h, cheeky_gaze_provider.h);
// no Cheeky source is included here.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "aer.h"
#include "camera_interceptor.h"
#include "dlss_probe.h"
#include "eye_gaze.h"
#include "logger.h"
#include "openvr_gaze.h"
#include "perf_stats.h"
#include "settings.h"

namespace {

struct GazeView {   // CheekyGazeViewV1
    uint32_t structure_size, view_index, flags, array_index;
    float center_u, center_v;
    int32_t image_rect_x, image_rect_y;
    uint32_t image_rect_width, image_rect_height;
    uint64_t resource_identity, swapchain_identity;
    float next_jump_u, next_jump_v;
    float fov_left, fov_right, fov_up, fov_down;
    float forward_u, forward_v;
};
struct GazeSnapshot {   // CheekyGazeSnapshotV1
    uint32_t abi_version, structure_size, status_flags, view_count;
    uint64_t sequence, publication_qpc;
    int64_t predicted_display_time, sample_time;
    uint64_t session_generation, swapchain_generation;
    char runtime_name[128];
    GazeView views[2];
};
constexpr uint32_t kAbi = 4;
// Status and view flags (Cheeky's gaze ABI).
constexpr uint32_t kLayerActive = 1u << 0, kExtensionEnabled = 1u << 1, kSystemSupported = 1u << 2, kFocused = 1u << 3,
                   kActionActive = 1u << 4, kGazeValid = 1u << 5, kMappingReady = 1u << 6;
constexpr uint32_t kOrientationValid = 1u << 0, kOrientationTracked = 1u << 2, kFovValid = 1u << 6, kForwardValid = 1u << 7;

std::atomic<uint64_t> g_seq{0};
std::atomic<uint64_t> g_gazeStamp{1};

// What Cheeky was actually given, every 10 s: snapshot calls, how many carried a valid gaze, eye-view calls answered.
std::mutex g_diagMutex;
uint64_t g_snapCalls = 0, g_snapValid = 0, g_viewCalls = 0, g_viewOk = 0;
double g_lastDiag = 0.0;

void HostDiag(bool snapshot, bool ok) {
    std::lock_guard<std::mutex> lock(g_diagMutex);
    if (snapshot) { ++g_snapCalls; g_snapValid += ok ? 1 : 0; }
    else { ++g_viewCalls; g_viewOk += ok ? 1 : 0; }
    const double now = vrmod::NowMs();
    if (g_lastDiag == 0.0) g_lastDiag = now;
    if (now - g_lastDiag < 10000.0) return;
    g_lastDiag = now;
    char b[240];
    snprintf(b, sizeof(b), "Cheeky host (10 s): gaze snapshots %llu, with a valid gaze %llu | eye-view requests %llu, answered %llu "
             "(answered only with per-eye DLSS running in gameplay)",
             static_cast<unsigned long long>(g_snapCalls), static_cast<unsigned long long>(g_snapValid),
             static_cast<unsigned long long>(g_viewCalls), static_cast<unsigned long long>(g_viewOk));
    modlog::Line(b);
    g_snapCalls = g_snapValid = g_viewCalls = g_viewOk = 0;
}   // the snapshot's sample stamp (see CheekyGazeProvider_GetGazeSnapshot)

} // namespace

struct CheekyGazeProviderEyeViewsV1 {
    uint32_t version, structure_size;
    uint64_t left_view, right_view;          // the game's NGX feature handles
    uint32_t output_width, output_height;    // DLSS output size
    uint64_t session_generation;             // matches the gaze snapshot's
};

extern "C" __declspec(dllexport) uint32_t __cdecl CheekyGazeProvider_GetGazeSnapshot(uint32_t version, void* output, uint32_t size) {
    if (version != kAbi || !output || size < sizeof(GazeSnapshot)) return 0;
    GazeSnapshot s{};
    s.abi_version = kAbi;
    s.structure_size = sizeof(GazeSnapshot);
    s.view_count = 2;
    s.sequence = ++g_seq;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    s.publication_qpc = static_cast<uint64_t>(q.QuadPart);
    // Cheeky takes a gaze sample only when this stamp is non-zero and has advanced since the last one it saw (it ages the
    // sample from the moment the stamp last changed). An opaque counter, as Cheeky's own OpenVR adapter uses.
    s.session_generation = 1;
    s.swapchain_generation = 1;
    strcpy_s(s.runtime_name, "SOTTR VR mod (AER host)");
    // The rendered frustum: the game's final vertical FOV and aspect of the latest frame.
    float fovV = 0.0f, aspect = 0.0f;
    const uint64_t f = vrmod::CameraLatestGameFrame();
    if (!(vrmod::AerFinalProjectionOf(f, &fovV, &aspect) || vrmod::AerFinalProjectionOf(f - 1, &fovV, &aspect) ||
          vrmod::AerFinalProjectionOf(f - 2, &fovV, &aspect))) {
        fovV = 1.4f;
        aspect = 1.387f;
    }
    const float tanV = std::tan(fovV * 0.5f), tanH = tanV * aspect;
    // Each eye's gaze: SteamVR's OpenVR foveation centre per eye, else the OpenXR gaze (one direction for both).
    float u[2] = { 0.5f, 0.5f }, v[2] = { 0.5f, 0.5f };
    // [foveation] mode: fixed (1) reports no gaze, so Cheeky keeps the sharp region in the middle; eye (2) passes it on.
    bool gaze = vrmod::SettingGet("foveation.mode") >= 2.0f;
    for (int e = 0; gaze && e < 2; ++e) {
        float d[3];
        uint64_t stamp = 0;
        bool have = vrmod::OpenVrGazeLatest(e, d, 100.0, &stamp);
        if (have) {
            // Offset so the stamp is never 0.
            if (e == 0) g_gazeStamp.store(stamp + 1, std::memory_order_relaxed);
        } else if ((have = vrmod::EyeGazeLatest(d, 100.0))) {
            if (e == 0) g_gazeStamp.fetch_add(1, std::memory_order_relaxed);
        }
        if (!(have && d[2] < -0.05f)) {
            gaze = false;
            break;
        }
        u[e] = 0.5f + 0.5f * (d[0] / -d[2]) / tanH;
        v[e] = 0.5f - 0.5f * (d[1] / -d[2]) / tanV;
        u[e] = u[e] < 0.0f ? 0.0f : (u[e] > 1.0f ? 1.0f : u[e]);
        v[e] = v[e] < 0.0f ? 0.0f : (v[e] > 1.0f ? 1.0f : v[e]);
    }
    if (!gaze) u[0] = u[1] = v[0] = v[1] = 0.5f;
    s.status_flags = kLayerActive | kExtensionEnabled | kSystemSupported | kFocused | kActionActive | kMappingReady | (gaze ? kGazeValid : 0u);
    uint64_t left = 0, right = 0;
    uint32_t ow = 0, oh = 0;
    vrmod::DlssEyeHandles(&left, &right, &ow, &oh);
    for (uint32_t e = 0; e < 2; ++e) {
        GazeView& gv = s.views[e];
        gv.structure_size = sizeof(GazeView);
        gv.view_index = e;
        gv.flags = kOrientationValid | kOrientationTracked | kFovValid | kForwardValid;
        gv.center_u = u[e];
        gv.center_v = v[e];
        gv.next_jump_u = u[e];
        gv.next_jump_v = v[e];
        gv.image_rect_x = 0;
        gv.image_rect_y = 0;
        gv.image_rect_width = ow;
        gv.image_rect_height = oh;
        gv.fov_left = -std::atan(tanH);
        gv.fov_right = std::atan(tanH);
        gv.fov_up = std::atan(tanV);
        gv.fov_down = -std::atan(tanV);
        gv.forward_u = 0.5f;
        gv.forward_v = 0.5f;
    }
    s.predicted_display_time = static_cast<int64_t>(g_gazeStamp.load(std::memory_order_relaxed));
    s.sample_time = s.predicted_display_time;
    memcpy(output, &s, sizeof s);
    HostDiag(true, gaze);
    return 1;
}

extern "C" __declspec(dllexport) uint32_t __cdecl CheekyGazeProvider_GetEyeViews(CheekyGazeProviderEyeViewsV1* out) {
    if (!out || out->structure_size < sizeof(CheekyGazeProviderEyeViewsV1)) return 0;
    uint64_t left = 0, right = 0;
    uint32_t ow = 0, oh = 0;
    if (!vrmod::DlssEyeHandles(&left, &right, &ow, &oh)) { HostDiag(false, false); return 0; }
    HostDiag(false, true);
    out->version = 1;
    out->left_view = left;
    out->right_view = right;
    out->output_width = ow;
    out->output_height = oh;
    out->session_generation = 1;
    return 1;
}
