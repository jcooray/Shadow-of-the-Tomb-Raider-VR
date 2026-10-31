#include "build_config.h"
#include "openxr_integration.h"

#include <windows.h>
#include <cstdlib>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <iomanip>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>

#include "logger.h"
#include "eye_split.h"
#include "head_tracking.h"
#include "camera_interceptor.h"
#include "stall_watchdog.h"
#include "perf_stats.h"
#include "bmp_writer.h"
#include "audio_cue.h"
#include "desktop_spoof.h"
#include "gpu_frame_timer.h"
#include "settings.h"
#include "gpu_tier.h"
#include "steamvr_settings.h"
#include "frame_log.h"
#include "d3d12_debug.h"
#include "ghost_test.h"
#include "pairing_test.h"
#include "reader_probe.h"
#include "facing_probe.h"
#include "value_scan.h"
#include "counter_scan.h"
#include "aer.h"
#include "projection_probe.h"
#include "vr_overlay.h"
#include "cinematic_watch.h"
#include "bone_scan.h"
#include "near_probe.h"
#include "ui_probe.h"
#include "aim_finder.h"
#include "aim_watch.h"
#include "dlss_probe.h"
#include "eye_gaze.h"
#include "gear_hide.h"
#include "cam_state_probe.h"
#include "head_drive.h"
#include "afw.h"
#include "hud_layer.h"
#include "resolution_unlock.h"

namespace vrmod {

namespace {

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_systemId = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_appSpace = XR_NULL_HANDLE;
XrSwapchain g_swapchainLeft = XR_NULL_HANDLE;
XrSwapchain g_swapchainRight = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D11KHR> g_swapchainImagesLeft;
std::vector<XrSwapchainImageD3D11KHR> g_swapchainImagesRight;
std::vector<XrSwapchainImageD3D12KHR> g_swapchainImages12Left;
std::vector<XrSwapchainImageD3D12KHR> g_swapchainImages12Right;

// ---- D3D12 path (the game running in DX12: SBS backbuffer copied per eye) ----
// Everything runs on the GAME's command queue, so our copy is ordered after all
// of the game's work for the frame, and the OpenXR runtime (given the same queue
// in the session binding) orders its own use of the images after our copy.
bool g_useD3D12 = false;
ID3D12CommandQueue* g_gameQueue = nullptr;        // captured at swapchain creation
ID3D12Device* g_device12 = nullptr;

// Eye tint (Ctrl+U, Phase 1): render-target views onto the OpenXR swapchain images, created on demand.
// Invalidated whenever the swapchains are (re)created.
constexpr int kTintRtvMax = 32;
ID3D12DescriptorHeap* g_tintRtvHeap = nullptr;
UINT g_tintRtvStride = 0;
ID3D12Resource* g_tintRtvRes[kTintRtvMax] = {};
int g_tintRtvCount = 0;
constexpr int kFramesInFlight3 = 3;
ID3D12CommandAllocator* g_allocators[kFramesInFlight3] = {};
ID3D12GraphicsCommandList* g_cmdList = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceValue = 0;
UINT64 g_allocatorFence[kFramesInFlight3] = {};
int g_allocatorIndex = 0;
PhaseStat g_xrWaitFrame, g_xrWaitImage, g_xrThrottle, g_xrEndFrame;
bool g_pacingEarly = true;   // see SubmitStereoFrame; F7 toggles
bool g_halfRate = false;     // End toggles: game renders every other display slot
bool g_eyeGazeExtensionEnabled = false;   // XR_EXT_eye_gaze_interaction offered by the runtime (probe only)

// Phase 8: headset refresh rate (XR_FB_display_refresh_rate, offered by SteamVR 2.x and the Quest runtimes).
bool g_refreshExt = false;
PFN_xrEnumerateDisplayRefreshRatesFB g_xrEnumerateRefreshRates = nullptr;
PFN_xrGetDisplayRefreshRateFB g_xrGetRefreshRate = nullptr;
PFN_xrRequestDisplayRefreshRateFB g_xrRequestRefreshRate = nullptr;
std::vector<float> g_refreshRates;   // supported, ascending
std::string g_runtimeName, g_systemName;   // for the overlay's Status page
LUID g_adapterLuid{};                       // the graphics card the runtime renders on (its graphics requirements)
float g_refreshHz = 0.0f;            // current, as reported by the runtime (0 = unknown)

// Screen mode (Delete toggles): the frame shown on a flat virtual screen so the HUD, menus and map fit.
bool g_screenMode = false;
constexpr float kMenuScreenScale = 0.75f;   // menus/map are drawn at 75% of the normal view size
XrPosef g_screenPose{ {0, 0, 0, 1}, {0, 0, -1.8f} };
float g_screenDistance = 1.8f;   // metres in front of you when switched on
std::atomic<int> g_screenIntroRequest{ -1 };   // -1 none, 1 on, 0 off (OpenXRScreenModeForIntro)
bool g_screenForIntro = false;                 // screen mode is on because of the opening, not the player
float g_screenWidth = 2.6f;      // metres wide (about 72 degrees of view at 1.8 m)

// Frame-timing trace (needs XR_KHR_win32_convert_performance_counter_time).
PFN_xrConvertTimeToWin32PerformanceCounterKHR g_convertTime = nullptr;
struct FrameRec {
    double waitReturnMs = 0;   // when xrWaitFrame returned for this frame
    double endCallMs = 0;      // when we called xrEndFrame
    double displayMs = 0;      // predicted display time, in the same clock
    double periodMs = 0;
    bool shouldRender = true;
};
FrameRec g_frameRing[8];
int g_frameRingPos = 0;
double g_lastDisplayMs = 0;
int g_skipEventsLogged = 0;
int g_skipsInWindow = 0;
int g_slotsLostInWindow = 0;

double XrTimeToMs(XrTime t) {
    LARGE_INTEGER li{};
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    if (!g_convertTime || XR_FAILED(g_convertTime(g_instance, t, &li))) {
        return 0.0;
    }
    return static_cast<double>(li.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}
int g_maxFramesAhead = 1;   // D3D12: frames the GPU may still owe when the CPU moves on; 0 = no limit. Key ' cycles.
DXGI_FORMAT g_backbufferFormat = DXGI_FORMAT_UNKNOWN;
uint32_t g_eyeWidth = 0;
uint32_t g_eyeHeight = 0;
uint32_t g_recommendedW = 0, g_recommendedH = 0;   // xrEnumerateViewConfigurationViews, per eye

// [render] mode=mono (D3D12 only): the backbuffer is one full frame, not two squeezed halves, so each eye image is
// the whole backbuffer and both eyes get the same one. SBS (the default) is unchanged.
bool MonoFrames() {
    return g_useD3D12 && (GetRenderMode() == RenderMode::Mono || GetRenderMode() == RenderMode::Aer);
}

// [render] mode=aer (D3D12): each presented frame is one eye's full-frame image (see aer.h).
bool AerFrames() {
    return g_useD3D12 && GetRenderMode() == RenderMode::Aer;
}

// Alternate-eye rendering: the pose each eye's swapchain image was rendered with (declared every frame, also while
// that eye's image is not updated), and whether it is known.
PoseF g_aerEyePose[2];
bool g_aerEyeValid[2] = { false, false };

// Eye-image width for a backbuffer of this width: all of it in mono, half of it in SBS.
uint32_t EyeWidthForBackbuffer(uint64_t backbufferWidth) {
    return static_cast<uint32_t>(MonoFrames() ? backbufferWidth : backbufferWidth / 2);
}

// How many eye images side by side make up the frame the game projected (for its aspect).
float FrameWidthInEyes() {
    return MonoFrames() ? 1.0f : 2.0f;
}

// The OpenXR swapchains are created ONCE at a fixed capacity and never torn down
// on a resolution change: destroying big swapchains while the compositor still
// holds them coincided with three NVIDIA driver TDRs/bugchecks when the game
// shrank its resolution. The eye image is copied into the top-left corner and
// submitted with a sub-rectangle (imageRect) of just that size.
constexpr uint32_t kSwapchainMinWidth = 3840;    // 7680x4320 half-SBS eye
constexpr uint32_t kSwapchainMinHeight = 4320;
uint32_t g_swapWidth = 0;
uint32_t g_swapHeight = 0;
int64_t g_swapchainFormat = 0;
bool g_swapchainsValid = false;
bool g_sessionRunning = false;
bool g_initialized = false;
bool g_initFailed = false;

// Held for the whole of UpdateOpenXR so ShutdownOpenXR can't destroy the
// session mid-frame.
std::timed_mutex g_xrMutex;
std::atomic<bool> g_shuttingDown{false};

bool XrCheck(XrResult result, const char* what) {
    if (XR_FAILED(result)) {
        modlog::Line("OpenXR: " + std::string(what) + " failed, XrResult=" + std::to_string(result));
        return false;
    }
    return true;
}

bool PickSwapchainFormat(int64_t* outFormat) {
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g_session, count, &count, formats.data());

    std::ostringstream oss;
    oss << "OpenXR: supported swapchain formats:";
    for (auto f : formats) {
        oss << " " << f;
    }
    modlog::Line(oss.str());

    if (g_useD3D12) {
        // CopyTextureRegion needs the source and destination in the same format
        // family, so the swapchain must match the game's backbuffer exactly
        // (or be the sRGB view of the same 8-bit layout).
        DXGI_FORMAT srgbTwin = DXGI_FORMAT_UNKNOWN;
        if (g_backbufferFormat == DXGI_FORMAT_R8G8B8A8_UNORM) srgbTwin = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (g_backbufferFormat == DXGI_FORMAT_B8G8R8A8_UNORM) srgbTwin = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if (g_backbufferFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) srgbTwin = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (g_backbufferFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) srgbTwin = DXGI_FORMAT_B8G8R8A8_UNORM;
        for (auto f : formats) {
            if (f == g_backbufferFormat) { *outFormat = f; return true; }
        }
        for (auto f : formats) {
            if (srgbTwin != DXGI_FORMAT_UNKNOWN && f == srgbTwin) { *outFormat = f; return true; }
        }
        modlog::Line("OpenXR: no swapchain format compatible with backbuffer format " + std::to_string(g_backbufferFormat));
        return false;
    }

    for (auto f : formats) {
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM) {
            *outFormat = f;
            return true;
        }
    }
    if (!formats.empty()) {
        *outFormat = formats[0];
        return true;
    }
    return false;
}

bool CreateEyeSwapchain(int64_t format, XrSwapchain* outSwapchain, std::vector<XrSwapchainImageD3D11KHR>* outImages) {
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    createInfo.format = format;
    createInfo.width = g_swapWidth;
    createInfo.height = g_swapHeight;
    createInfo.sampleCount = 1;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;

    if (!XrCheck(xrCreateSwapchain(g_session, &createInfo, outSwapchain), "xrCreateSwapchain")) {
        return false;
    }

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(*outSwapchain, 0, &imageCount, nullptr);
    if (g_useD3D12) {
        auto* images12 = (outSwapchain == &g_swapchainLeft) ? &g_swapchainImages12Left : &g_swapchainImages12Right;
        g_tintRtvCount = 0;   // cached tint views may point at the old images
        images12->resize(imageCount, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(*outSwapchain, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images12->data()));
    } else {
        outImages->resize(imageCount, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(*outSwapchain, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(outImages->data()));
    }

    modlog::Line("OpenXR: swapchain created with " + std::to_string(imageCount) + " images.");
    return true;
}

// For the D3D12 path the caller sets g_useD3D12 / g_device12 / g_gameQueue /
// g_backbufferFormat first, and passes the per-eye size (half the SBS backbuffer).
bool InitOpenXR(ID3D11Device* device, uint32_t eyeW12 = 0, uint32_t eyeH12 = 0) {
    if (g_initialized) {
        return true;
    }
    if (g_initFailed) {
        return false;
    }

    std::vector<const char*> extensions = { g_useD3D12 ? XR_KHR_D3D12_ENABLE_EXTENSION_NAME : XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    {
        // Optional: lets us convert XrTime to QueryPerformanceCounter time for frame-timing diagnostics.
        uint32_t extCount = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
        std::vector<XrExtensionProperties> props(extCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, props.data());
        std::string all;
        for (auto& p : props) {
            all += std::string(" ") + p.extensionName;
            if (strcmp(p.extensionName, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0) {
                extensions.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
            }
            // Enabled only to ask the runtime whether it offers eye gaze (harmless; nothing uses it yet).
            if (strcmp(p.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) {
                extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
                g_eyeGazeExtensionEnabled = true;
            }
            if (strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) {
                extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
                g_refreshExt = true;
            }
        }
        modlog::Line("OpenXR: runtime instance extensions:" + all);
    }
    XrInstanceCreateInfo instanceCreateInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    instanceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceCreateInfo.enabledExtensionNames = extensions.data();
    strcpy_s(instanceCreateInfo.applicationInfo.applicationName, "SOTTR-VR-Mod");
    // Declare a conservative 1.0.x API version rather than XR_CURRENT_API_VERSION
    // (whatever the fetched SDK headers report, e.g. 1.1.63 from the "main"
    // branch) -- SteamVR's OpenXR runtime only formally supports the 1.0.x
    // spec and rejected 1.1.x outright with XR_ERROR_API_VERSION_UNSUPPORTED.
    instanceCreateInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);

    // CheekyFoveatedDLSS's own OpenXR layer, when installed system-wide (e.g. for a UEVR game), would load into our
    // instance too; the Cheeky fork then ignores this mod's per-eye views and falls back to its marker calibration,
    // which can't follow alternate-eye rendering (2026-09-30: fovea stuck in the centre). This mod supplies Cheeky's gaze
    // and eye views itself, so keep the layer out of this process through the variable its manifest names
    // ("disable_environment"); the loader reads it at instance creation. [gaze] cheeky_openxr_layer=1 lets it load.
    if (!GetCheekyOpenXrLayerAllowed()) {
        SetEnvironmentVariableW(L"CHEEKY_OPENXR_LAYER_DISABLE", L"1");
        _putenv_s("CHEEKY_OPENXR_LAYER_DISABLE", "1");
        modlog::Line("OpenXR: Cheeky's OpenXR layer disabled for this process (this mod hosts Cheeky's gaze and eye views).");
    }
    if (!XrCheck(xrCreateInstance(&instanceCreateInfo, &g_instance), "xrCreateInstance")) {
        g_initFailed = true;
        return false;
    }
    modlog::Line("OpenXR: instance created.");
    {
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &ip))) {
            std::ostringstream o;
            o << ip.runtimeName << ' ' << XR_VERSION_MAJOR(ip.runtimeVersion) << '.' << XR_VERSION_MINOR(ip.runtimeVersion)
              << '.' << XR_VERSION_PATCH(ip.runtimeVersion);
            modlog::Line("OpenXR: runtime " + o.str());
            g_runtimeName = o.str();
            FrameLogSetMeta("openxr_runtime", o.str());
        }
    }
    xrGetInstanceProcAddr(g_instance, "xrConvertTimeToWin32PerformanceCounterKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&g_convertTime));
    modlog::Line(g_convertTime ? "OpenXR: time conversion available (frame timing trace on)."
                               : "OpenXR: time conversion NOT available (no frame timing trace).");
    if (g_refreshExt) {
        xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction*>(&g_xrEnumerateRefreshRates));
        xrGetInstanceProcAddr(g_instance, "xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&g_xrGetRefreshRate));
        xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&g_xrRequestRefreshRate));
        g_refreshExt = g_xrEnumerateRefreshRates && g_xrGetRefreshRate && g_xrRequestRefreshRate;
    }
    modlog::Line(g_refreshExt ? "PACE: headset refresh-rate control available (XR_FB_display_refresh_rate)."
                              : "PACE: headset refresh-rate control NOT available (set the rate in the runtime's own settings).");

    XrSystemGetInfo systemGetInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrCheck(xrGetSystem(g_instance, &systemGetInfo, &g_systemId), "xrGetSystem")) {
        g_initFailed = true;
        return false;
    }
    modlog::Line("OpenXR: system acquired (headset found).");
    {
        XrSystemEyeGazeInteractionPropertiesEXT eyeProps{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
        XrSystemProperties sysProps{XR_TYPE_SYSTEM_PROPERTIES};
        if (g_eyeGazeExtensionEnabled) {
            sysProps.next = &eyeProps;
        }
        if (XR_SUCCEEDED(xrGetSystemProperties(g_instance, g_systemId, &sysProps))) {
            g_systemName = sysProps.systemName;
            // For the next start's log (LogSystemInfo), which may end before VR begins.
            SettingsWriteKeys({ { "render.headset", g_systemName + " (" + g_runtimeName + ")" } });
            modlog::Line(std::string("OpenXR: system '") + sysProps.systemName + "', vendor id " + std::to_string(sysProps.vendorId)
                + ", eye gaze extension " + (g_eyeGazeExtensionEnabled ? "offered" : "NOT offered by the runtime")
                + (g_eyeGazeExtensionEnabled ? (eyeProps.supportsEyeGazeInteraction ? ", headset reports eye tracking SUPPORTED" : ", headset reports eye tracking NOT supported") : ""));
        }
    }

    XrGraphicsBindingD3D11KHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrGraphicsBindingD3D12KHR graphicsBinding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    void* bindingChain = nullptr;

    if (g_useD3D12) {
        PFN_xrGetD3D12GraphicsRequirementsKHR getD3D12Requirements = nullptr;
        xrGetInstanceProcAddr(g_instance, "xrGetD3D12GraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&getD3D12Requirements));
        if (!getD3D12Requirements) {
            modlog::Line("OpenXR: failed to load xrGetD3D12GraphicsRequirementsKHR.");
            g_initFailed = true;
            return false;
        }
        XrGraphicsRequirementsD3D12KHR requirements12{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        if (!XrCheck(getD3D12Requirements(g_instance, g_systemId, &requirements12), "xrGetD3D12GraphicsRequirementsKHR")) {
            g_initFailed = true;
            return false;
        }
        g_adapterLuid = requirements12.adapterLuid;
        // Bind to the game's own device AND its command queue (the queue the
        // swapchain presents on), so our copy and the runtime's later use of the
        // images are ordered on the same queue.
        graphicsBinding12.device = g_device12;
        graphicsBinding12.queue = g_gameQueue;
        bindingChain = &graphicsBinding12;
    } else {
        PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11Requirements = nullptr;
        xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&getD3D11Requirements));
        if (!getD3D11Requirements) {
            modlog::Line("OpenXR: failed to load xrGetD3D11GraphicsRequirementsKHR.");
            g_initFailed = true;
            return false;
        }
        XrGraphicsRequirementsD3D11KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (!XrCheck(getD3D11Requirements(g_instance, g_systemId, &graphicsRequirements), "xrGetD3D11GraphicsRequirementsKHR")) {
            g_initFailed = true;
            return false;
        }
        g_adapterLuid = graphicsRequirements.adapterLuid;

        // Bind the session to the game's own D3D11 device -- not a throwaway one,
        // since the swapchain images need to be usable (CopyResource) alongside
        // the game's own textures without a cross-device copy.
        graphicsBinding.device = device;
        bindingChain = &graphicsBinding;
    }

    XrSessionCreateInfo sessionCreateInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = bindingChain;
    sessionCreateInfo.systemId = g_systemId;
    if (!XrCheck(xrCreateSession(g_instance, &sessionCreateInfo, &g_session), "xrCreateSession")) {
        g_initFailed = true;
        return false;
    }
    modlog::Line("OpenXR: session created.");
    if (g_eyeGazeExtensionEnabled) EyeGazeInitSession(g_instance, g_session, g_systemId);

    XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceCreateInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!XrCheck(xrCreateReferenceSpace(g_session, &spaceCreateInfo, &g_appSpace), "xrCreateReferenceSpace")) {
        g_initFailed = true;
        return false;
    }

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    if (viewCount != 2) {
        modlog::Line("OpenXR: unexpected view count " + std::to_string(viewCount) + " (expected 2 for stereo).");
        g_initFailed = true;
        return false;
    }
    XrViewConfigurationView viewConfigs[2]{};
    viewConfigs[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    viewConfigs[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    xrEnumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, viewConfigs);

    g_recommendedW = viewConfigs[0].recommendedImageRectWidth;
    g_recommendedH = viewConfigs[0].recommendedImageRectHeight;
    modlog::Line("OpenXR: runtime-recommended per-eye size "
        + std::to_string(viewConfigs[0].recommendedImageRectWidth) + "x"
        + std::to_string(viewConfigs[0].recommendedImageRectHeight)
        + " (Phase 6 derives the render size from it; see PHASE6 lines).");

    // Create the swapchain at OUR eye texture's actual size (from the game's
    // own SBS split, 1920x2160), not the runtime's recommendation (Pimax
    // Dream Air's native per-eye panel res, 4184x3256). CopyResource requires
    // matching dimensions and fails silently on mismatch -- that mismatch is
    // exactly what produced a black headset view the first time this ran.
    // Composing a layer below native panel resolution is normal; the
    // compositor scales it up.
    if (g_useD3D12) {
        g_eyeWidth = eyeW12;
        g_eyeHeight = eyeH12;
    } else {
        ID3D11Texture2D* leftEyeTexture = GetLeftEyeTexture();
        if (!leftEyeTexture) {
            modlog::Line("OpenXR: eye_split hasn't created its textures yet, can't size the swapchain.");
            g_initFailed = true;
            return false;
        }
        D3D11_TEXTURE2D_DESC eyeDesc{};
        leftEyeTexture->GetDesc(&eyeDesc);
        g_eyeWidth = eyeDesc.Width;
        g_eyeHeight = eyeDesc.Height;
    }
    modlog::Line("OpenXR: using actual eye texture size " + std::to_string(g_eyeWidth) + "x" + std::to_string(g_eyeHeight));

    int64_t swapchainFormat = 0;
    if (!PickSwapchainFormat(&swapchainFormat)) {
        modlog::Line("OpenXR: no usable swapchain format found.");
        g_initFailed = true;
        return false;
    }

    g_swapchainFormat = swapchainFormat;
    g_swapWidth = (std::max)(g_eyeWidth, kSwapchainMinWidth);
    g_swapHeight = (std::max)(g_eyeHeight, kSwapchainMinHeight);
    modlog::Line("OpenXR: swapchain capacity " + std::to_string(g_swapWidth) + "x" + std::to_string(g_swapHeight)
        + " (eye images are copied into a sub-rectangle).");
    if (!CreateEyeSwapchain(swapchainFormat, &g_swapchainLeft, &g_swapchainImagesLeft)
        || !CreateEyeSwapchain(swapchainFormat, &g_swapchainRight, &g_swapchainImagesRight)) {
        g_initFailed = true;
        return false;
    }
    g_swapchainsValid = true;

    if (g_useD3D12) {
        bool ok = SUCCEEDED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (int i = 0; ok && i < kFramesInFlight3; ++i) {
            ok = SUCCEEDED(g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocators[i])));
        }
        ok = ok && g_fenceEvent
            && SUCCEEDED(g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], nullptr, IID_PPV_ARGS(&g_cmdList)));
        if (ok && g_gameQueue) {
            GpuFrameTimerStart(g_gameQueue, g_fence);
        }
        if (!ok) {
            modlog::Line("OpenXR: failed to create D3D12 copy command objects.");
            g_initFailed = true;
            return false;
        }
        g_cmdList->Close();
        modlog::Line("OpenXR: D3D12 path ready (queue-ordered backbuffer copy).");
    }

    g_initialized = true;
    modlog::Line("OpenXR: initialization complete.");
    return true;
}

std::string RefreshList() {
    std::ostringstream o;
    for (size_t i = 0; i < g_refreshRates.size(); ++i) o << (i ? ", " : "") << g_refreshRates[i];
    return o.str();
}

// Asks the runtime for the supported rate nearest `hz`. The change is confirmed by the refresh-changed event.
void RequestRefresh(float hz) {
    if (!g_refreshExt || g_refreshRates.empty() || hz <= 0.0f) return;
    float best = g_refreshRates[0];
    for (float r : g_refreshRates) if (std::fabs(r - hz) < std::fabs(best - hz)) best = r;
    const XrResult res = g_xrRequestRefreshRate(g_session, best);
    std::ostringstream o;
    o << "PACE: requested headset refresh " << best << " Hz (current " << g_refreshHz << " Hz): "
      << (XR_SUCCEEDED(res) ? "accepted" : "REFUSED, XrResult " + std::to_string(static_cast<int>(res)));
    modlog::Line(o.str());
    if (XR_SUCCEEDED(res)) SetRefreshHzForSave(best);
}

// Once the session runs: log the supported rates and the current one; apply [render] refresh_hz if set.
void RefreshSetup() {
    if (!g_refreshExt) return;
    uint32_t n = 0;
    if (XR_SUCCEEDED(g_xrEnumerateRefreshRates(g_session, 0, &n, nullptr)) && n > 0) {
        g_refreshRates.assign(n, 0.0f);
        if (XR_FAILED(g_xrEnumerateRefreshRates(g_session, n, &n, g_refreshRates.data()))) g_refreshRates.clear();
        g_refreshRates.resize(g_refreshRates.empty() ? 0 : n);
        std::sort(g_refreshRates.begin(), g_refreshRates.end());
    }
    float cur = 0.0f;
    if (XR_SUCCEEDED(g_xrGetRefreshRate(g_session, &cur))) g_refreshHz = cur;
    modlog::Line("PACE: headset refresh rates offered: " + (g_refreshRates.empty() ? std::string("none") : RefreshList())
        + " Hz; current " + std::to_string(g_refreshHz) + " Hz; [render] refresh_hz=" + std::to_string(GetRefreshHz()));
    if (GetRefreshHz() > 0.0f && std::fabs(GetRefreshHz() - g_refreshHz) > 0.5f) RequestRefresh(GetRefreshHz());
}

void PollXrEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            const auto* e = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
            g_refreshHz = e->toDisplayRefreshRate;
            std::ostringstream o;
            o << "PACE: headset refresh changed " << e->fromDisplayRefreshRate << " -> " << e->toDisplayRefreshRate << " Hz";
            modlog::Line(o.str());
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* stateEvent = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            modlog::Line("OpenXR: session state changed to " + std::to_string(stateEvent->state));

            if (stateEvent->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrCheck(xrBeginSession(g_session, &beginInfo), "xrBeginSession");
                g_sessionRunning = true;
                modlog::Line("OpenXR: session begun, rendering active.");
                RefreshSetup();
            } else if (stateEvent->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                g_sessionRunning = false;
            } else if (stateEvent->state == XR_SESSION_STATE_EXITING
                       || stateEvent->state == XR_SESSION_STATE_LOSS_PENDING) {
                g_sessionRunning = false;
            }
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

void CopyIntoSwapchain(XrSwapchain swapchain, std::vector<XrSwapchainImageD3D11KHR>& images,
                        ID3D11Texture2D* srcTexture, ID3D11DeviceContext* context) {
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XrCheck(xrAcquireSwapchainImage(swapchain, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage")) {
        return;
    }

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    StallWatchdogSetPhase("xr:xrWaitSwapchainImage");
    if (!XrCheck(xrWaitSwapchainImage(swapchain, &waitInfo), "xrWaitSwapchainImage")) {
        return;
    }

    // Same-format region copy into the top-left of the (larger, fixed-size)
    // swapchain image; a plain CopyResource would need identical dimensions.
    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTexture->GetDesc(&srcDesc);
    D3D11_BOX box{0, 0, 0, srcDesc.Width, srcDesc.Height, 1};
    context->CopySubresourceRegion(images[imageIndex].texture, 0, 0, 0, 0, srcTexture, 0, &box);

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrCheck(xrReleaseSwapchainImage(swapchain, &releaseInfo), "xrReleaseSwapchainImage");
}

// The game's eye images are symmetric-frustum, 2:1 horizontally squeezed
// (half-SBS) renders: the 16:9 projection squeezed into half the width, so the
// declared frustum is half-tan V = tan(fov/2), half-tan H = V * 16/9. Declaring
// the runtime's asymmetric per-eye FOV instead put each image's centre ~4.3 deg
// off the optical axis (~8.6 deg of forced divergence). The FOV is the one the
// game camera is actually rendering with: the game's own 55 deg (0.9599) by
// default, or our forced value while head tracking owns the camera. Trim live
// (punctuation keys -- no numpad assumed):
//   . / ,  : shift both images inward / outward (convergence)
//   ] / [  : increase / decrease the FOV forced into the game camera
//   /      : swap left/right eye sources     ;  : reset all
constexpr float kGameNativeFov = 0.959931f;
float g_convergenceShift = 0.0f;
bool g_swapEyes = false;

// The projection aspect is the FULL backbuffer's (both eyes squeezed into half
// the width each), so it follows the resolution the game is actually running
// at -- 16:9 at 3840x2160, 4:3 at 5760x4320 and so on. Tunable live in case a
// non-16:9 mode makes the game treat the FOV differently:
//   F1 / F2 : declared aspect -1% / +1% (held keys repeat)     F3 : reset
float g_aspectTune = 1.0f;

float SqueezedProjectionAspect() {
    if (g_eyeHeight == 0) {
        return 16.0f / 9.0f;
    }
    return FrameWidthInEyes() * static_cast<float>(g_eyeWidth) / static_cast<float>(g_eyeHeight) * g_aspectTune;
}

// Which axis the game's FOV value pins when the backbuffer aspect changes. The
// game's 16:9 behaviour is verified (vertical FOV = value, horizontal = *16/9),
// but a 5760x4320 (4:3) frame warped under head tracking, so the projection is
// not simply "vertical fixed" at other aspects. Models (key \ cycles):
//   0 : vertical FOV fixed at every aspect
//   1 : below 16:9 the HORIZONTAL FOV stays at its 16:9 value (vertical grows);
//       at or above 16:9 vertical stays fixed          (default; matches 4:3 warp)
//   2 : horizontal FOV fixed at its 16:9 value at every aspect
//   3 : the game's own projection aspect, 1.775, at every backbuffer size (default). The game's log prints
//       "Aspect Ratio: 1.775 (Pixel Aspect Ratio: 1.0016)" even with a 6048x5376 monitor rect: it takes the aspect from
//       the monitor, not the resolution, so a 16:9 picture is always squeezed into the per-eye image.
int g_fovModel = 3;
constexpr float kGameProjectionAspect = 1.775f;

void ComputeHalfTans(float renderFov, float* outHalfTanH, float* outHalfTanV) {
    constexpr float kRefAspect = 16.0f / 9.0f;
    float aspect = SqueezedProjectionAspect();
    float t = std::tan(renderFov * 0.5f);
    static bool modelChosen = false;
    if (!modelChosen) {
        modelChosen = true;
        // With the monitor aspect spoofed to the render shape the game projects at that aspect, so start from the
        // aspect-following model (1) instead of the fixed 1.775 one; `\` cycles them if the picture is warped.
        if (DesktopSpoofAspectOverridden()) g_fovModel = 1;
    }
    if (g_fovModel == 3) {
        *outHalfTanV = t;
        *outHalfTanH = t * kGameProjectionAspect * g_aspectTune;
        return;
    }
    bool pinHorizontal = (g_fovModel == 2) || (g_fovModel == 1 && aspect < kRefAspect);
    if (pinHorizontal) {
        *outHalfTanH = t * kRefAspect;
        *outHalfTanV = *outHalfTanH / aspect;
    } else {
        *outHalfTanV = t;
        *outHalfTanH = t * aspect;
    }
}

// ---- Phase 7: AER frame-loop statistics (logged every 3600 Presents as AERSTAT) ----
double g_lastWaitFrameMs = std::nan("");   // how long the xrWaitFrame that produced the current frame state blocked

// Phase 8: why a Present was not shown as a normal alternate-eye frame. Also the recorder's `fallback` column.
enum Fallback : char {
    kFbNone = 0,
    kFbMenu = 'm',        // menu, map, loading screen or video
    kFbNoTracking = 't',  // head tracking off
    kFbUnpaired = 'u',    // no AER write known for the presented frame
    kFbStalled = 's',     // the game stopped writing the camera (no new game frame for kStallPresents Presents)
    kFbForeign = 'f',     // something else wrote the camera between our writes: our pose is not what renders
    kFbCutscene = 'c',    // in-engine cutscene with [render] aer_cutscenes=mono
    kFbForced = 'v',      // Ctrl+V
    kFbMonoFrame = 'o',   // a frame rendered from the head centre, still in the pipeline after a mono request ended
    kFbResync = 'y',      // Phase 7 resync (the pairing clamp moved)
};
const char* FallbackName(char f) {
    switch (f) {
    case kFbMenu: return "menu/map/loading/video";
    case kFbNoTracking: return "head tracking off";
    case kFbUnpaired: return "no AER frame known";
    case kFbStalled: return "game stopped writing the camera";
    case kFbForeign: return "camera written by something else (writes not honoured)";
    case kFbCutscene: return "cutscene (aer_cutscenes=mono)";
    case kFbForced: return "forced (Ctrl+V)";
    case kFbMonoFrame: return "head-centre frame in the pipeline";
    case kFbResync: return "resync";
    default: return "none";
    }
}

struct AerStats {
    uint64_t presents = 0, left = 0, right = 0, resyncs = 0, repeats = 0, frameRepeats = 0, frameSkips = 0;
    uint64_t menu = 0, noTracking = 0, unpaired = 0, stalled = 0, foreign = 0, cutscene = 0, forced = 0, monoFrame = 0;
    uint64_t cinematic = 0, cinematicStereo = 0;   // Presents during an in-engine cutscene, and how many of them stereo
    double waitSum = 0.0, waitMax = 0.0, predSum = 0.0, predAbsMax = 0.0;
    uint64_t waitN = 0, predN = 0;
    int lastEye = -1;
    uint64_t lastFrame = 0;
    uint64_t foreignWrites0 = 0, otherWrites0 = 0, adjusted0 = 0;   // totals at the window start
};
AerStats g_aerStats;

// routed: an eye image was shown normally this Present; eye: that eye (or -1); frame: the game frame shown;
// why: the fallback reason when not routed.
void AerStatsAdd(bool routed, int eye, uint64_t frame, double predErr, double waitMs, char why, bool cinematic) {
    AerStats& s = g_aerStats;
    if (s.presents == 0) {
        s.foreignWrites0 = CameraForeignWrites();
        s.otherWrites0 = CameraOtherStructWrites();
        s.adjusted0 = CameraAdjustedWrites();
    }
    ++s.presents;
    switch (why) {
    case kFbMenu: ++s.menu; break;
    case kFbNoTracking: ++s.noTracking; break;
    case kFbUnpaired: ++s.unpaired; break;
    case kFbStalled: ++s.stalled; break;
    case kFbForeign: ++s.foreign; break;
    case kFbCutscene: ++s.cutscene; break;
    case kFbForced: ++s.forced; break;
    case kFbMonoFrame: ++s.monoFrame; break;
    default: break;   // resyncs are counted where they happen
    }
    if (cinematic) {
        ++s.cinematic;
        if (routed) ++s.cinematicStereo;
    }
    if (routed) {
        (eye == 0 ? s.left : s.right) += 1;
        if (s.lastEye == eye) ++s.repeats;   // the same eye twice in a row
        if (s.lastFrame) {
            if (frame == s.lastFrame) ++s.frameRepeats;
            else if (frame > s.lastFrame + 1) ++s.frameSkips;
        }
        s.lastEye = eye;
        s.lastFrame = frame;
    } else {
        s.lastEye = -1;
        s.lastFrame = 0;
    }
    if (!std::isnan(waitMs)) { s.waitSum += waitMs; s.waitMax = (std::max)(s.waitMax, waitMs); ++s.waitN; }
    if (!std::isnan(predErr)) { s.predSum += predErr; s.predAbsMax = (std::max)(s.predAbsMax, std::fabs(predErr)); ++s.predN; }
    if (s.presents % 3600 == 0) {
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << "AERSTAT (last 3600 Presents): left " << s.left << ", right " << s.right
          << ", resyncs " << s.resyncs << ", same eye twice " << s.repeats << ", game frame repeated " << s.frameRepeats
          << ", skipped " << s.frameSkips << " | xrWaitFrame mean " << (s.waitN ? s.waitSum / s.waitN : 0.0) << " ms, max " << s.waitMax
          << " ms | pose prediction error (display - predicted-for) mean " << (s.predN ? s.predSum / s.predN : 0.0) << " ms, max |" << s.predAbsMax
          << "| ms (aer_predict_frames " << GetAerPredictFrames() << ") | split frames merged (total) " << CameraSplitMerges();
        modlog::Line(o.str());
        std::ostringstream m;
        m << "AERSTAT mono Presents: menu " << s.menu << ", head tracking off " << s.noTracking << ", unpaired " << s.unpaired
          << ", game stopped writing " << s.stalled << ", writes not honoured " << s.foreign << ", cutscene " << s.cutscene
          << ", forced " << s.forced << ", head-centre frames " << s.monoFrame
          << " | cutscene Presents " << s.cinematic << " (stereo " << s.cinematicStereo << ")"
          << " | foreign camera writes " << (CameraForeignWrites() - s.foreignWrites0)
          << ", camera adjusted by the game " << (CameraAdjustedWrites() - s.adjusted0) << " (max " << CameraTakeAdjustMaxCm() << " cm)"
          << ", game writes to other camera structs " << (CameraOtherStructWrites() - s.otherWrites0);
        modlog::Line(m.str());
        const int lastEye = s.lastEye;
        const uint64_t lastFrame = s.lastFrame;
        s = AerStats{};
        s.presents = 0;
        s.lastEye = lastEye;
        s.lastFrame = lastFrame;
    }
}

// ---- Phase 8: pacing (logged every 3600 Presents as PACE, every mode) ----
// AER needs the game to hold the headset's refresh rate: each eye updates every other frame, so a missed frame leaves
// one eye's image older than the other's (uneven alternation, which reads as swimming).
//   display step: how far the predicted display time advanced since the previous Present, in display periods
//                 (1 = on time; k > 1 = k - 1 headset frames missed).
//   eye age:      at each Present, how many periods old each eye's image is at display (steady AER: 0 and 1).
//   wait:         how long xrWaitFrame blocked -- the slack the frame had (near 0 = at the limit).
struct PaceStats {
    uint64_t presents = 0, onTime = 0, lateSteps = 0, missedFrames = 0, longestMiss = 0;
    uint64_t age1 = 0, age2 = 0, age3 = 0;   // max eye age <= 1, == 2, >= 3 periods
    std::vector<float> waits;
};
PaceStats g_pace;
int64_t g_paceLastDisplay = 0;
int64_t g_eyeUpdatedDisplay[2] = { 0, 0 };

// Returns the display step (periods) and fills each eye's age at display (periods; NaN when unknown).
// counted: false in menus, maps, loading and with head tracking off (a load stall is not a pacing problem).
int PaceAdd(int64_t displayNs, int64_t periodNs, int eyeMask, double waitMs, float age[2], bool counted) {
    PaceStats& s = g_pace;
    int step = 0;
    if (g_paceLastDisplay > 0 && periodNs > 0) {
        step = static_cast<int>(std::llround(static_cast<double>(displayNs - g_paceLastDisplay) / static_cast<double>(periodNs)));
    }
    g_paceLastDisplay = displayNs;
    for (int e = 0; e < 2; ++e) {
        if (eyeMask & (1 << e)) g_eyeUpdatedDisplay[e] = displayNs;
        age[e] = (g_eyeUpdatedDisplay[e] > 0 && periodNs > 0)
            ? static_cast<float>(static_cast<double>(displayNs - g_eyeUpdatedDisplay[e]) / static_cast<double>(periodNs)) : std::nanf("");
    }
    if (!counted) return step;
    ++s.presents;
    if (step == 1) ++s.onTime;
    if (step > 1) {
        ++s.lateSteps;
        s.missedFrames += static_cast<uint64_t>(step - 1);
        s.longestMiss = (std::max)(s.longestMiss, static_cast<uint64_t>(step - 1));
    }
    if (!std::isnan(age[0]) && !std::isnan(age[1])) {
        const float mx = (std::max)(age[0], age[1]);
        if (mx < 1.5f) ++s.age1; else if (mx < 2.5f) ++s.age2; else ++s.age3;
    }
    if (!std::isnan(waitMs)) s.waits.push_back(static_cast<float>(waitMs));
    if (s.presents % 3600 == 0) {
        std::sort(s.waits.begin(), s.waits.end());
        auto pct = [&](double q) { return s.waits.empty() ? 0.0f : s.waits[static_cast<size_t>(q * (s.waits.size() - 1))]; };
        const double periodMs = static_cast<double>(periodNs) / 1.0e6;
        const double missPct = 100.0 * static_cast<double>(s.missedFrames) / static_cast<double>(s.presents + s.missedFrames);
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << "PACE (last 3600 gameplay Presents): headset " << (g_refreshHz > 0.0f ? g_refreshHz : static_cast<float>(1000.0 / periodMs))
          << " Hz (period " << periodMs << " ms) | on time " << s.onTime << ", late " << s.lateSteps << " (headset frames missed "
          << s.missedFrames << " = " << missPct << " %, longest " << s.longestMiss << ")"
          << " | oldest eye image at display: <=1 period " << s.age1 << ", 2 periods " << s.age2 << ", 3+ " << s.age3
          << " | xrWaitFrame slack p5 " << pct(0.05) << " / p50 " << pct(0.5) << " ms";
        modlog::Line(o.str());
        // Brief: prefer a lower headset refresh held steadily over a fluctuating frame rate.
        if (missPct > 1.0) {
            float lower = 0.0f;
            for (float r : g_refreshRates) if (r < g_refreshHz - 0.5f) lower = r;
            std::ostringstream a;
            a << "PACE: the game missed " << missPct << " % of headset frames -- uneven eye alternation. Recommend a lower refresh"
              << (lower > 0.0f ? " (" + std::to_string(static_cast<int>(lower + 0.5f)) + " Hz: Ctrl+B cycles, or [render] refresh_hz)" : " in the runtime's settings")
              << ".";
            modlog::Line(a.str());
        }
        s = PaceStats{};
    }
    return step;
}

// Ctrl+B: next lower supported refresh rate (wraps to the highest). Ctrl+V: force the Phase 8 mono fallback (AER).
bool g_forceMono = false;
void PacePollHotkeys() {
    static bool bWas = false, vWas = false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool b = (VRMOD_DEV_KEY('B') & 0x8000) != 0, v = (VRMOD_DEV_KEY('V') & 0x8000) != 0;
    if (b && !bWas && ctrl && !alt) {
        if (!g_refreshExt || g_refreshRates.size() < 2) {
            modlog::Line(g_refreshRates.size() == 1
                ? "PACE: Ctrl+B -- the runtime offers only " + RefreshList() + " Hz through OpenXR; set other rates in the headset's own software."
                : std::string("PACE: Ctrl+B -- the runtime offers no refresh-rate control."));
            PlayCue(Cue::Failure);
        } else {
            float next = g_refreshRates.back();
            for (float r : g_refreshRates) if (r < g_refreshHz - 0.5f) next = r;   // the highest rate below the current
            if (!(next < g_refreshHz - 0.5f)) next = g_refreshRates.back();
            RequestRefresh(next);
            PlayCue(Cue::On);
        }
    }
    if (v && !vWas && ctrl && !alt && AerFrames()) {
        g_forceMono = !g_forceMono;
        modlog::Line(std::string("AER: forced mono ") + (g_forceMono ? "ON (Ctrl+V; head-centre frames to both eyes)" : "off"));
        PlayCue(g_forceMono ? Cue::On : Cue::Off);
    }
    bWas = b;
    vWas = v;
}
// ---- Phase 6: projection match (AER) ----
// The game renders a symmetric frustum; the runtime's views are asymmetric. The game's FOV is set so its frustum
// covers the symmetric cover of both eyes' views (half-angles = max of |left|,|right| and of |up|,|down|, times the
// overscan), and each eye declares exactly the frustum its image was rendered with (from the FOV value written for
// that frame). Under every FOV model the game frustum is linear in tan(fov/2), so the FOV follows from the model's
// coefficients at tan = 1. Which model is right is measured by the projection probe (Ctrl+O).
float g_aerEyeFov[2] = { 0.0f, 0.0f };   // FOV value each eye's current image was rendered with
bool g_aerEyeHaveFinal[2] = { false, false };   // ...and the game's own final projection for it, when known
float g_aerEyeFinalFov[2] = { 0.0f, 0.0f }, g_aerEyeFinalAspect[2] = { 0.0f, 0.0f };
uint64_t g_pmCorrected = 0;   // eye-frames whose final projection differed from the FOV model by > 0.5 %
float g_pmGameFov = 0.0f;                 // FOV value currently written into the game camera (AER)
float g_pmLoggedFov = 0.0f;
uint64_t g_pmFrames = 0, g_pmMismatches = 0;

void UpdateProjectionMatch(const XrView views[2]) {
    float tH = 0.0f, tV = 0.0f;
    for (int e = 0; e < 2; ++e) {
        tH = (std::max)(tH, (std::max)(std::tan(std::fabs(views[e].fov.angleLeft)), std::tan(std::fabs(views[e].fov.angleRight))));
        tV = (std::max)(tV, (std::max)(std::tan(std::fabs(views[e].fov.angleUp)), std::tan(std::fabs(views[e].fov.angleDown))));
    }
    const float overscan = GetRenderOverscan();
    tH *= overscan;
    tV *= overscan;
    float aH = 1.0f, aV = 1.0f;
    ComputeHalfTans(1.5707963f, &aH, &aV);   // coefficients of the game frustum at tan(fov/2) = 1
    const float t = (std::max)(tH / aH, tV / aV);
    const float fov = 2.0f * std::atan(t);
    g_pmGameFov = fov;
    AerSetGameFov(fov);
    if (g_pmLoggedFov > 0.0f && std::fabs(fov - g_pmLoggedFov) < 1e-3f * g_pmLoggedFov) return;
    g_pmLoggedFov = fov;
    // Target render size: the runtime's recommended per-eye pixel density (at the centre), uniform (square pixels),
    // spread over the symmetric cover. SteamVR's own resolution setting (global, manual or Auto -- 150 % on an RTX 5090 --
    // times this game's per-app one) is in the size it recommends: taken back out, so the target is SteamVR's 100 %
    // whatever it is set to and the mod's Render resolution alone decides. Other runtimes (VDXR, Oculus, ...) have no such
    // setting; a runtime's quality preset that is part of its 100 % (Virtual Desktop's) stays in.
    double steamVr = 1.0;
    if (g_runtimeName.find("SteamVR") != std::string::npos) {
        const SteamVrScale s = ReadSteamVrScale();
        if (s.read && s.Total() > 0.1) steamVr = s.Total();
    }
    const float l0 = std::tan(std::fabs(views[0].fov.angleLeft)), r0 = std::tan(std::fabs(views[0].fov.angleRight));
    const float u0 = std::tan(std::fabs(views[0].fov.angleUp)), d0 = std::tan(std::fabs(views[0].fov.angleDown));
    const float density = (std::max)(g_recommendedW / (l0 + r0), g_recommendedH / (u0 + d0)) / static_cast<float>(std::sqrt(steamVr));
    const uint32_t tw = static_cast<uint32_t>(2.0f * tH * density / 8.0f + 0.5f) * 8;
    const uint32_t th = static_cast<uint32_t>(2.0f * tV * density / 8.0f + 0.5f) * 8;
    const float gH = t * aH, gV = t * aV;
    auto deg = [](float r) { return r * 57.2957795f; };
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "PHASE6: runtime FOV (deg, left/right/up/down): eye0 " << deg(views[0].fov.angleLeft) << "/" << deg(views[0].fov.angleRight) << "/"
      << deg(views[0].fov.angleUp) << "/" << deg(views[0].fov.angleDown) << ", eye1 " << deg(views[1].fov.angleLeft) << "/"
      << deg(views[1].fov.angleRight) << "/" << deg(views[1].fov.angleUp) << "/" << deg(views[1].fov.angleDown)
      << " | symmetric cover half-angles " << deg(std::atan(tH)) << " x " << deg(std::atan(tV)) << " deg (overscan " << overscan << ")"
      << " | game FOV value " << std::setprecision(4) << fov << " rad (" << std::setprecision(2) << deg(fov) << " deg), fov model " << g_fovModel
      << " -> game frustum half-angles " << deg(std::atan(gH)) << " x " << deg(std::atan(gV)) << " deg (tan " << std::setprecision(4) << gH << " x " << gV
      << ", excess over cover H " << std::setprecision(1) << 100.0f * (gH / tH - 1.0f) << " %, V " << 100.0f * (gV / tV - 1.0f) << " %)"
      << " | backbuffer " << g_eyeWidth << "x" << g_eyeHeight << " (aspect " << std::setprecision(3) << static_cast<float>(g_eyeWidth) / (std::max)(1u, g_eyeHeight)
      << ") | target render size " << tw << "x" << th << " (aspect " << tH / tV << ") from recommended " << g_recommendedW << "x" << g_recommendedH
      << " with SteamVR's resolution setting (" << std::setprecision(0) << steamVr * 100.0 << " %) taken out" << std::setprecision(2)
      << " -- set vrmod_fakedesktop.txt and the game resolution to it";
    modlog::Line(o.str());
    // Kept in the settings: the overlay's "Render resolution" is a percentage of it (applied at the next start). Only a
    // real change is stored (> 2 %, e.g. SteamVR's resolution setting): the runtime's recommended size wobbles by a few
    // pixels between sessions (4180-4192), which moved the 150 % size between 6336 / 6352 / 6368 (2026-10-21).
    const float storedW = SettingGet("render.target_w"), storedH = SettingGet("render.target_h");
    const bool targetChanged = storedW < 640.0f || std::fabs(tw - storedW) > 0.02f * storedW || std::fabs(th - storedH) > 0.02f * storedH;
    if (targetChanged) {
        SettingSet("render.target_w", static_cast<float>(tw));
        SettingSet("render.target_h", static_cast<float>(th));
        SettingsWriteKeys({ { "render.target_w", std::to_string(tw) }, { "render.target_h", std::to_string(th) } });
    }
    // A new install (or until the overlay sets it): the render resolution is chosen for the graphics card and this target
    // -- the runtime's own quality setting (e.g. Virtual Desktop's Godlike) is already in the target size -- and the
    // foveated rendering; chosen again when either changes (another headset, Setup run again with other foveation).
    {
        const Foveation fov = CurrentFoveation();
        if (SettingGet("render.resolution_auto") != 0.0f && (targetChanged || SettingGet("render.auto_foveation") != static_cast<float>(fov))) {
            const GpuInfo gpu = GpuInfoForLuid(g_adapterLuid);
            const int pct = StartingResolutionPct(gpu.relative, tw, th, fov);
            const double ms = PredictedFrameMs(gpu.relative, tw, th, pct, fov);
            const bool slow = ms > 1000.0 / 72.0;
            std::ostringstream rel;
            rel << gpu.relative;
            SettingSet("render.resolution_pct", static_cast<float>(pct));
            SettingSet("render.gpu_note", slow ? 1.0f : 0.0f);
            SettingSet("render.gpu_relative", gpu.relative);
            SettingSet("render.auto_foveation", static_cast<float>(fov));
            SettingsWriteKeys({ { "render.resolution_pct", std::to_string(pct) }, { "render.gpu_note", slow ? "1" : "0" },
                                { "render.gpu_relative", rel.str() }, { "render.auto_foveation", std::to_string(static_cast<int>(fov)) } });
            std::ostringstream r;
            r << "RESOLUTION: graphics card '" << (gpu.name.empty() ? std::string("not found") : gpu.name) << "' (" << gpu.vramMB << " MB, ~"
              << static_cast<int>(gpu.relative * 100.0f + 0.5f) << " % of an RTX 5090" << (gpu.known ? "" : ", not in the table: a mid-range guess")
              << "), headset target " << tw << "x" << th << ", foveated rendering " << FoveationName(fov)
              << " -> starting render resolution " << pct << " % (chosen for the card;"
              << " the overlay's Render resolution changes it), expected GPU time " << std::fixed << std::setprecision(1) << ms << " ms"
              << (slow ? " -- over a 72 Hz frame even so: the headset will suggest lower game settings" : "");
            modlog::Line(r.str());
        }
    }
}
// The game can change resolution while running (in-menu, or via the injected
// modes). CopyResource silently does nothing on a size mismatch, so the
// swapchains must follow the eye textures or the headset goes black again.
void RecreateSwapchainsIfEyeSizeChanged(uint32_t eyeW, uint32_t eyeH) {
    struct { uint32_t Width, Height; } d{ eyeW, eyeH };
    if (g_swapchainsValid && d.Width == g_eyeWidth && d.Height == g_eyeHeight) {
        return;
    }
    if (!g_swapchainsValid && d.Width == g_eyeWidth && d.Height == g_eyeHeight) {
        return; // already failed once at this size; don't retry every frame
    }

    if (g_swapchainsValid && d.Width <= g_swapWidth && d.Height <= g_swapHeight) {
        // Fits the fixed-capacity swapchains: just change the submitted sub-rectangle.
        modlog::Line("OpenXR: eye texture size is now " + std::to_string(d.Width) + "x" + std::to_string(d.Height)
            + " (was " + std::to_string(g_eyeWidth) + "x" + std::to_string(g_eyeHeight)
            + "); swapchains kept at " + std::to_string(g_swapWidth) + "x" + std::to_string(g_swapHeight) + ".");
        g_eyeWidth = d.Width;
        g_eyeHeight = d.Height;
        return;
    }

    modlog::Line("OpenXR: eye texture size is now " + std::to_string(d.Width) + "x" + std::to_string(d.Height)
        + " (exceeds swapchain capacity " + std::to_string(g_swapWidth) + "x" + std::to_string(g_swapHeight) + "), recreating.");
    g_swapWidth = (std::max)(g_swapWidth, d.Width);
    g_swapHeight = (std::max)(g_swapHeight, d.Height);
    g_swapchainsValid = false;
    if (g_swapchainLeft != XR_NULL_HANDLE) { xrDestroySwapchain(g_swapchainLeft); g_swapchainLeft = XR_NULL_HANDLE; }
    if (g_swapchainRight != XR_NULL_HANDLE) { xrDestroySwapchain(g_swapchainRight); g_swapchainRight = XR_NULL_HANDLE; }
    g_swapchainImagesLeft.clear();
    g_swapchainImagesRight.clear();
    g_swapchainImages12Left.clear();
    g_swapchainImages12Right.clear();

    g_eyeWidth = d.Width;
    g_eyeHeight = d.Height;
    if (CreateEyeSwapchain(g_swapchainFormat, &g_swapchainLeft, &g_swapchainImagesLeft)
        && CreateEyeSwapchain(g_swapchainFormat, &g_swapchainRight, &g_swapchainImagesRight)) {
        g_swapchainsValid = true;
        modlog::Line("OpenXR: swapchains recreated.");
    } else {
        modlog::Line("OpenXR: swapchain recreation FAILED, not submitting frames until the size changes again.");
    }
}

void UpdateTuningHotkeys() {
    auto down = [](int vk) {   // release build: only the modifiers are read (build_config.h)
        const bool real = vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT;
        return ((real ? GetAsyncKeyState(vk) : VRMOD_DEV_KEY(vk)) & 0x8000) != 0;
    };
    static bool dirty = false;
    bool changed = false;

    if (down(VK_OEM_PERIOD)) { g_convergenceShift += 0.0005f; changed = true; }
    if (down(VK_OEM_COMMA)) { g_convergenceShift -= 0.0005f; changed = true; }
    const bool modifier = down(VK_CONTROL) || down(VK_MENU);   // Ctrl+[ / Ctrl+] are the AER world scale
    if (!modifier && down(VK_OEM_6)) { HeadTrackingAdjustFov(1.002f); changed = true; }   // ]
    if (!modifier && down(VK_OEM_4)) { HeadTrackingAdjustFov(1.0f / 1.002f); changed = true; }   // [
    if (down(VK_OEM_1)) { g_convergenceShift = 0.0f; g_swapEyes = false; g_aspectTune = 1.0f; HeadTrackingResetTuning(); changed = true; }   // ;
    if (down(VK_F1)) { g_aspectTune /= 1.002f; changed = true; }
    if (down(VK_F2)) { g_aspectTune *= 1.002f; changed = true; }
    if (down(VK_F3)) { g_aspectTune = 1.0f; changed = true; }

    static bool swapWasDown = false;
    bool swapDown = down(VK_OEM_2);   // /
    if (swapDown && !swapWasDown) {
        g_swapEyes = !g_swapEyes;
        changed = true;
    }
    swapWasDown = swapDown;

    static bool modelWasDown = false;
    bool modelDown = down(VK_OEM_5);   // backslash
    if (modelDown && !modelWasDown) {
        g_fovModel = (g_fovModel + 1) % 4;
        changed = true;
    }
    modelWasDown = modelDown;

    static bool aheadWasDown = false;
    bool aheadDown = down(VK_OEM_7);   // apostrophe/quote key
    if (aheadDown && !aheadWasDown) {
        g_maxFramesAhead = (g_maxFramesAhead + 1) % 4;   // 0 (off), 1, 2, 3
        changed = true;
    }
    aheadWasDown = aheadDown;

    if (changed) {
        dirty = true;
    } else if (dirty) {
        dirty = false;
        std::ostringstream oss;
        oss << "OpenXR tuning: convergenceShift=" << g_convergenceShift
            << " gameFov=" << HeadTrackingGetGameFov() << " swapEyes=" << g_swapEyes
            << " framesAhead=" << g_maxFramesAhead << " fovModel=" << g_fovModel << " aspectTune=" << g_aspectTune << " declaredAspect=" << SqueezedProjectionAspect()
            << " eye=" << g_eyeWidth << "x" << g_eyeHeight;
        modlog::Line(oss.str());
    }
}

// Render-target view for a swapchain image (for the eye tint), created on first use.
bool TintRtvFor(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    if (!g_tintRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kTintRtvMax;
        if (FAILED(g_device12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_tintRtvHeap)))) {
            g_tintRtvHeap = nullptr;
            return false;
        }
        g_tintRtvStride = g_device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    int slot = -1;
    for (int i = 0; i < g_tintRtvCount; ++i) {
        if (g_tintRtvRes[i] == res) slot = i;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_tintRtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (slot < 0) {
        if (g_tintRtvCount == kTintRtvMax) g_tintRtvCount = 0;
        slot = g_tintRtvCount++;
        g_tintRtvRes[slot] = res;
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = static_cast<DXGI_FORMAT>(g_swapchainFormat);   // typed even if the image itself is typeless
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        h.ptr += static_cast<SIZE_T>(slot) * g_tintRtvStride;
        g_device12->CreateRenderTargetView(res, &rd, h);
    } else {
        h.ptr += static_cast<SIZE_T>(slot) * g_tintRtvStride;
    }
    *out = h;
    return true;
}

// D3D12: copies the two halves of the game's SBS backbuffer into the two OpenXR
// swapchain images on the game's own queue (ordered after the game's frame). eyeMask (bit 0 left, bit 1 right)
// limits the update to those eyes' swapchains: alternate-eye rendering updates one eye per frame, and the other
// eye's swapchain keeps its last released image (which the compositor keeps showing, with that image's pose).
// AFW request for this Present (set by the AER pairing just before CopyBackbufferToEyes12): the eye that was rendered,
// its game frame, and f_px * baseline (cm) for the other eye's warp.
bool g_afwReq = false;
int g_afwEye = 0;
uint64_t g_afwFrame = 0;
float g_afwBaselineCm = 0.0f, g_afwTanH = 0.0f, g_afwTanV = 0.0f;

bool CopyBackbufferToEyes12(ID3D12Resource* backbuffer, int eyeMask = 3) {
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

    uint32_t idx[2] = {};
    XrSwapchain chains[2] = { g_swapchainLeft, g_swapchainRight };
    bool acquired[2] = {};
    bool ok = true;
    const double acquireStart = NowMs();
    for (int e = 0; e < 2 && ok; ++e) {
        if (!(eyeMask & (1 << e))) continue;
        ok = XrCheck(xrAcquireSwapchainImage(chains[e], &acquireInfo, &idx[e]), "xrAcquireSwapchainImage(12)");
        acquired[e] = ok;
        if (ok) {
            StallWatchdogSetPhase("xr:xrWaitSwapchainImage");
            ok = XrCheck(xrWaitSwapchainImage(chains[e], &waitInfo), "xrWaitSwapchainImage(12)");
        }
    }

    g_xrWaitImage.Add(NowMs() - acquireStart);

    if (ok) {
        const int a = g_allocatorIndex;
        g_allocatorIndex = (g_allocatorIndex + 1) % kFramesInFlight3;
        if (g_fence->GetCompletedValue() < g_allocatorFence[a]) {
            g_fence->SetEventOnCompletion(g_allocatorFence[a], g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 2000);
        }
        g_allocators[a]->Reset();
        g_cmdList->Reset(g_allocators[a], nullptr);

        ID3D12Resource* dst[2] = { (eyeMask & 1) ? g_swapchainImages12Left[idx[0]].texture : nullptr,
                                   (eyeMask & 2) ? g_swapchainImages12Right[idx[1]].texture : nullptr };

        D3D12_RESOURCE_BARRIER pre[3] = {};
        auto transition = [](D3D12_RESOURCE_BARRIER& b, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = r;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
        };
        UINT nb = 0;
        transition(pre[nb++], backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (int e = 0; e < 2; ++e) if (dst[e]) transition(pre[nb++], dst[e], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        g_cmdList->ResourceBarrier(nb, pre);

        for (int e = 0; e < 2; ++e) {
            if (!dst[e]) continue;
            if (g_afwReq && e != g_afwEye) continue;   // AFW: the other eye is warped below
            // e = destination eye; source half is swapped if the eyes are flipped. Mono: the whole frame to both.
            const uint32_t srcHalf = MonoFrames() ? 0u : (g_swapEyes ? (1 - e) : e);
            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = dst[e];
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = backbuffer;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = 0;
            D3D12_BOX box{ srcHalf * g_eyeWidth, 0, 0, (srcHalf + 1) * g_eyeWidth, g_eyeHeight, 1 };
            g_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
        }

        if (g_afwReq && dst[1 - g_afwEye] && !AfwRecordWarp(g_cmdList, backbuffer, dst[1 - g_afwEye], g_afwFrame, g_afwBaselineCm, g_afwTanH, g_afwTanV, g_afwEye)) {
            // Shouldn't happen (AfwReady was checked): the rendered image rather than nothing.
            D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
            dl.pResource = dst[1 - g_afwEye]; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sl.pResource = backbuffer; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_BOX box{ 0, 0, 0, g_eyeWidth, g_eyeHeight, 1 };
            g_cmdList->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
        }

        D3D12_RESOURCE_BARRIER post[3] = {};
        nb = 0;
        transition(post[nb++], backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        for (int e = 0; e < 2; ++e) if (dst[e]) transition(post[nb++], dst[e], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_cmdList->ResourceBarrier(nb, post);

        // HUD layer (hud_layer.cpp): the game's HUD, kept out of the backbuffer, over each eye -- after the warp, so it
        // isn't moved with the scene depth under it.
        HudLayerComposite(g_cmdList, dst, g_afwReq ? g_afwEye : (eyeMask == 1 ? 0 : eyeMask == 2 ? 1 : -1));

        // Ghost test (Ctrl+G): the view flips +-60 degrees up to 72 times a second, so the headset gets black images
        // instead. The test reads the backbuffer, which this doesn't touch.
        if (GhostTestBlackout() || PairingTestBlackout()) {
            static const FLOAT kBlack[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            for (int e = 0; e < 2; ++e) {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv;
                if (dst[e] && TintRtvFor(dst[e], &rtv)) {
                    g_cmdList->ClearRenderTargetView(rtv, kBlack, 0, nullptr);
                }
            }
        }

        // Eye tint (Ctrl+U): a solid block in each eye's image -- red in the image submitted to the left eye,
        // green in the right -- so the headset shows which frame reached which eye. The brief asks for a 64x64
        // corner; the corners of this headset are masked by the hidden-area mesh, so the block sits top-centre
        // and scales with the eye size (at least 64 px).
        if (FrameLogTintEnabled()) {
            static const FLOAT kColour[2][4] = { { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } };
            const LONG size = (std::max)(64L, static_cast<LONG>(g_eyeWidth / 24));
            const LONG x = static_cast<LONG>(g_eyeWidth / 2) - size / 2;
            const LONG y = static_cast<LONG>(g_eyeHeight / 4);
            const D3D12_RECT rect{ x, y, x + size, y + size };
            for (int e = 0; e < 2; ++e) {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv;
                if (dst[e] && TintRtvFor(dst[e], &rtv)) {
                    g_cmdList->ClearRenderTargetView(rtv, kColour[e], 1, &rect);
                }
            }
        }
        g_cmdList->Close();

        ID3D12CommandList* lists[] = { g_cmdList };
        GpuFrameTimerOurSubmit(true);
        g_gameQueue->ExecuteCommandLists(1, lists);
        GpuFrameTimerOurSubmit(false);
        g_gameQueue->Signal(g_fence, ++g_fenceValue);
        g_allocatorFence[a] = g_fenceValue;
        GpuFrameTimerFrameSubmitted(g_fenceValue, FrameLogCurrentFrame());
    }

    for (int e = 0; e < 2; ++e) {
        if (acquired[e]) {
            XrCheck(xrReleaseSwapchainImage(chains[e], &releaseInfo), "xrReleaseSwapchainImage(12)");
        }
    }

    // Limit how far the CPU may run ahead of the GPU. When the GPU is the
    // bottleneck (7680x4320), the game otherwise queues 2-3 frames, and every
    // queued frame is head-motion latency the compositor can't remove. Waiting
    // here for the frame N submissions ago keeps the queue that short.
    const double throttleStart = NowMs();
    if (ok && g_maxFramesAhead > 0 && g_fenceValue > static_cast<UINT64>(g_maxFramesAhead)) {
        const UINT64 waitFor = g_fenceValue - static_cast<UINT64>(g_maxFramesAhead);
        if (g_fence->GetCompletedValue() < waitFor) {
            g_fence->SetEventOnCompletion(waitFor, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 100);
        }
    }
    g_xrThrottle.Add(NowMs() - throttleStart);
    return ok;
}

// ---- In-headset overlay (vr_overlay.cpp): its panel as a quad layer over everything else ----
// A swapchain of its own (8-bit sRGB, so the GDI colours show as drawn), filled only when the panel image changes: the
// image is copied into a persistently mapped upload buffer and from there into the acquired swapchain image on the
// game's queue (our own allocator / list / fence). Between changes the layer keeps showing the last released image.
struct OverlayLayer {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    int w = 0, h = 0;
    bool rgba = false;                  // R8G8B8A8 (swizzle from the BGRA panel) instead of B8G8R8A8
    bool failed = false, haveImage = false, placed = false;
    ID3D12Resource* upload = nullptr;
    uint8_t* mapped = nullptr;
    UINT rowPitch = 0;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t fenceValue = 0;
    HANDLE event = nullptr;
    XrPosef pose{ { 0, 0, 0, 1 }, { 0, 0, -1 } };
} g_ov;

bool OverlayEnsure(int w, int h) {
    if (g_ov.failed) return false;
    if (g_ov.sc != XR_NULL_HANDLE) return g_ov.w == w && g_ov.h == h;
    auto fail = [](const char* why) {
        g_ov.failed = true;
        modlog::Line(std::string("OVERLAY: no headset panel -- ") + why);
        return false;
    };
    if (!g_useD3D12 || !g_device12 || !g_gameQueue) return fail("needs the game's D3D12 device");
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g_session, count, &count, formats.data());
    int64_t format = 0;
    for (int64_t want : { static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
                          static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM), static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM) }) {
        for (int64_t f : formats) if (f == want && !format) format = f;
    }
    if (!format) return fail("the runtime offers no 8-bit RGBA swapchain format");
    g_ov.rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_UNORM;
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format;
    ci.width = static_cast<uint32_t>(w);
    ci.height = static_cast<uint32_t>(h);
    ci.sampleCount = 1;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!XrCheck(xrCreateSwapchain(g_session, &ci, &g_ov.sc), "xrCreateSwapchain(overlay)")) { g_ov.sc = XR_NULL_HANDLE; return fail("xrCreateSwapchain failed"); }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_ov.sc, 0, &n, nullptr);
    g_ov.images.assign(n, XrSwapchainImageD3D12KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumerateSwapchainImages(g_ov.sc, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(g_ov.images.data()));
    g_ov.w = w;
    g_ov.h = h;
    g_ov.rowPitch = (static_cast<UINT>(w) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(g_ov.rowPitch) * h;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_device12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_ov.upload)))
        || FAILED(g_ov.upload->Map(0, nullptr, reinterpret_cast<void**>(&g_ov.mapped)))
        || FAILED(g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_ov.alloc)))
        || FAILED(g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ov.alloc, nullptr, IID_PPV_ARGS(&g_ov.list)))
        || FAILED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_ov.fence)))) {
        return fail("could not create its upload resources");
    }
    g_ov.list->Close();
    g_ov.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    modlog::Line("OVERLAY: headset panel ready (" + std::to_string(w) + " x " + std::to_string(h) + ", swapchain format "
                 + std::to_string(format) + ", " + std::to_string(n) + " images)");
    return true;
}

void OverlayUpload(const uint8_t* bgra, int w, int h) {
    if (!OverlayEnsure(w, h)) return;
    if (g_ov.fence->GetCompletedValue() < g_ov.fenceValue) {   // the previous upload is still on the GPU (rare: uploads are seconds apart)
        g_ov.fence->SetEventOnCompletion(g_ov.fenceValue, g_ov.event);
        if (WaitForSingleObject(g_ov.event, 100) != WAIT_OBJECT_0) return;
    }
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = bgra + static_cast<size_t>(y) * w * 4;
        uint8_t* dst = g_ov.mapped + static_cast<size_t>(y) * g_ov.rowPitch;
        if (!g_ov.rgba) {
            memcpy(dst, src, static_cast<size_t>(w) * 4);
        } else {
            for (int x = 0; x < w; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XrCheck(xrAcquireSwapchainImage(g_ov.sc, &ai, &idx), "xrAcquireSwapchainImage(overlay)")) return;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = 100000000;   // 100 ms
    if (!XrCheck(xrWaitSwapchainImage(g_ov.sc, &wi), "xrWaitSwapchainImage(overlay)")) {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(g_ov.sc, &ri);
        return;
    }
    ID3D12Resource* img = g_ov.images[idx].texture;
    g_ov.alloc->Reset();
    g_ov.list->Reset(g_ov.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = img;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_ov.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = img;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_ov.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = img->GetDesc().Format;
    src.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
    src.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = g_ov.rowPitch;
    g_ov.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_ov.list->ResourceBarrier(1, &b);
    g_ov.list->Close();
    ID3D12CommandList* lists[] = { g_ov.list };
    g_gameQueue->ExecuteCommandLists(1, lists);
    g_gameQueue->Signal(g_ov.fence, ++g_ov.fenceValue);
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    XrCheck(xrReleaseSwapchainImage(g_ov.sc, &ri), "xrReleaseSwapchainImage(overlay)");
    g_ov.haveImage = true;
}

// Places the panel: overlay.distance_m in front of the head's level forward direction, a little below eye height,
// turned to face the head.
void OverlayPlace(const XrView views[2]) {
    const XrQuaternionf& q = views[0].pose.orientation;
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float len = std::sqrt(fx * fx + fz * fz);
    float dx = 0.0f, dz = -1.0f;
    if (len > 0.05f) { dx = fx / len; dz = fz / len; }
    const float yaw = std::atan2(-dx, -dz);
    const float d = VrOverlayDistanceM();
    const XrVector3f head = { 0.5f * (views[0].pose.position.x + views[1].pose.position.x),
                              0.5f * (views[0].pose.position.y + views[1].pose.position.y),
                              0.5f * (views[0].pose.position.z + views[1].pose.position.z) };
    g_ov.pose.position = { head.x + dx * d, head.y - 0.12f * d, head.z + dz * d };
    g_ov.pose.orientation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    g_ov.placed = true;
}

void SubmitStereoFrame(ID3D11Texture2D* leftEyeTexture, ID3D11Texture2D* rightEyeTexture, ID3D12Resource* backbuffer12) {
    PollXrEvents();
    UpdateTuningHotkeys();
    HeadTrackingPollHotkeys();
    if (AerFrames()) AerPollHotkeys();
    PacePollHotkeys();

    if (!g_sessionRunning) {
        return;
    }
    if (g_useD3D12 ? !backbuffer12 : (!leftEyeTexture || !rightEyeTexture)) {
        return;
    }

    if (g_useD3D12) {
        D3D12_RESOURCE_DESC bd = backbuffer12->GetDesc();
        RecreateSwapchainsIfEyeSizeChanged(EyeWidthForBackbuffer(bd.Width), bd.Height);
    } else {
        D3D11_TEXTURE2D_DESC ld{};
        leftEyeTexture->GetDesc(&ld);
        RecreateSwapchainsIfEyeSizeChanged(ld.Width, ld.Height);
    }
    if (!g_swapchainsValid) {
        return;
    }

    // Frame pacing. "Late" (old): xrWaitFrame + xrBeginFrame run here in Present, right
    // before EndFrame, so the runtime measures our per-frame app time as ~0.3 ms
    // (the game's real work happened before the wait) and releases each frame as
    // late as it dares, leaving no slack -- any small delay then misses the
    // deadline and a whole 72 Hz slot is skipped (the log's one-in-six 27.8 ms
    // frames, also at 4K where the GPU is idle). "Early" (default): EndFrame
    // is followed immediately by the next xrWaitFrame/xrBeginFrame, so the game
    // renders the next frame *after* the runtime releases it and the runtime
    // sees the true app time.  F7 toggles.
    XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    static bool earlyFrameBegun = false;
    static XrFrameState earlyFrameState{XR_TYPE_FRAME_STATE};
    static bool f7WasDown = false;
    const bool f7Down = (VRMOD_DEV_KEY(VK_F7) & 0x8000) != 0;
    if (f7Down && !f7WasDown) {
        g_pacingEarly = !g_pacingEarly;
        earlyFrameBegun = false;
        modlog::Line(std::string("OpenXR: frame pacing is now ") + (g_pacingEarly ? "EARLY (wait after EndFrame)" : "LATE (wait before EndFrame)"));
    }
    f7WasDown = f7Down;

    static double earlyWaitReturnMs = 0.0;
    double waitReturnMs = 0.0;
    auto waitAndBegin = [&](XrFrameState* out, double* returnedAtMs) -> bool {
        StallWatchdogSetPhase("xr:xrWaitFrame");
        const double waitFrameStart = NowMs();
        if (!XrCheck(xrWaitFrame(g_session, &frameWaitInfo, out), "xrWaitFrame")) {
            return false;
        }
        *returnedAtMs = NowMs();
        g_xrWaitFrame.Add(*returnedAtMs - waitFrameStart);
        g_lastWaitFrameMs = *returnedAtMs - waitFrameStart;
        StallWatchdogSetPhase("xr:eyeGaze+beginFrame");
        EyeGazeUpdate(out->predictedDisplayTime);
        return XrCheck(xrBeginFrame(g_session, &frameBeginInfo), "xrBeginFrame");
    };

    if (g_pacingEarly) {
        if (!earlyFrameBegun) {
            if (!waitAndBegin(&earlyFrameState, &earlyWaitReturnMs)) {
                return;
            }
            earlyFrameBegun = true;
        }
        frameState = earlyFrameState;
        waitReturnMs = earlyWaitReturnMs;
    } else {
        if (!waitAndBegin(&frameState, &waitReturnMs)) {
            return;
        }
    }

    const double predictedDisplayMs = XrTimeToMs(frameState.predictedDisplayTime);
    FrameLogXrFrame(waitReturnMs, frameState.predictedDisplayTime, predictedDisplayMs,
        static_cast<double>(frameState.predictedDisplayPeriod) / 1.0e6, frameState.shouldRender != 0);
    {
        static uint32_t metaW = 0, metaH = 0;
        if (metaW != g_eyeWidth || metaH != g_eyeHeight) {
            metaW = g_eyeWidth;
            metaH = g_eyeHeight;
            FrameLogSetMeta("eye_size", std::to_string(g_eyeWidth) + "x" + std::to_string(g_eyeHeight));
            FrameLogSetMeta("swapchain_format", std::to_string(g_swapchainFormat));
            FrameLogSetMeta("mode", RenderModeName(GetRenderMode()));
            FrameLogSetEyeTag(MonoFrames() ? 'M' : 'S');
        }
    }

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quadLayers[2]{};
    XrCompositionLayerQuad overlayQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerProjectionView projViews[2]{};
    projViews[0].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    projViews[1].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

    if (frameState.shouldRender) {
        XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        viewLocateInfo.displayTime = frameState.predictedDisplayTime;
        viewLocateInfo.space = g_appSpace;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCountOutput = 0;
        XrView views[2]{ {XR_TYPE_VIEW}, {XR_TYPE_VIEW} };
        StallWatchdogSetPhase("xr:locateViews+pairing");
        XrCheck(xrLocateViews(g_session, &viewLocateInfo, &viewState, 2, &viewCountOutput, views), "xrLocateViews");

        bool poseValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
            && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (!poseValid) {
            // Fall back to a sane identity pose + a plausible symmetric FOV
            // instead of submitting whatever garbage/zeroed values the
            // runtime returned -- a zero FOV in particular produces a
            // degenerate frustum, which alone could explain "completely
            // disconnected, not converging" per eye.
            for (int eye = 0; eye < 2; ++eye) {
                views[eye].pose.orientation.w = 1.0f;
                views[eye].pose.position = { (eye == 0 ? -0.032f : 0.032f), 0.0f, 0.0f };
                views[eye].fov = { -0.75f, 0.75f, 0.75f, -0.75f }; // radians, ~85 deg symmetric
            }
        }

        static unsigned long long frameLogCount = 0;
        if (++frameLogCount % 300 == 1) {
            std::ostringstream oss;
            oss << "OpenXR: poseValid=" << poseValid;
            for (int eye = 0; eye < 2; ++eye) {
                oss << " | eye" << eye
                    << " pos=(" << views[eye].pose.position.x << "," << views[eye].pose.position.y << "," << views[eye].pose.position.z << ")"
                    << " orient=(" << views[eye].pose.orientation.x << "," << views[eye].pose.orientation.y << "," << views[eye].pose.orientation.z << "," << views[eye].pose.orientation.w << ")"
                    << " fov(l,r,u,d)=(" << views[eye].fov.angleLeft << "," << views[eye].fov.angleRight << "," << views[eye].fov.angleUp << "," << views[eye].fov.angleDown << ")";
            }
            modlog::Line(oss.str());
        }

        // Screen mode (Delete toggles): show the whole frame -- HUD, menus, map included --
        // as a flat 16:9 screen floating in front of you instead of filling your field of
        // view, so nothing at the edges is lost. The screen stays where you were looking
        // when you switched it on. The quad's aspect un-squeezes the half-SBS eye image.
        static bool deleteWasDown = false;
        const bool deleteDown = (VRMOD_DEV_KEY(VK_DELETE) & 0x8000) != 0;
        const bool manualToggle = (deleteDown && !deleteWasDown) || VrOverlayTakeScreenModeToggle();
        const int introRequest = g_screenIntroRequest.exchange(-1);
        bool toggle = manualToggle;
        if (manualToggle) {
            g_screenForIntro = false;   // the player's choice from now on
        } else if (introRequest == 1 && !g_screenMode) {
            toggle = g_screenForIntro = true;
        } else if (introRequest == 0 && g_screenMode && g_screenForIntro) {
            toggle = true;
            g_screenForIntro = false;
        }
        if (toggle) {
            g_screenMode = !g_screenMode;
            if (g_screenMode) {
                const XrQuaternionf& q = views[0].pose.orientation;
                const float fx = -2.0f * (q.x * q.z + q.w * q.y);
                const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                float len = std::sqrt(fx * fx + fz * fz);
                float dx = 0.0f, dz = -1.0f;
                if (len > 0.05f) { dx = fx / len; dz = fz / len; }
                const float yaw = std::atan2(-dx, -dz);
                const XrVector3f head = {
                    0.5f * (views[0].pose.position.x + views[1].pose.position.x),
                    0.5f * (views[0].pose.position.y + views[1].pose.position.y),
                    0.5f * (views[0].pose.position.z + views[1].pose.position.z) };
                g_screenPose.position = { head.x + dx * g_screenDistance, head.y, head.z + dz * g_screenDistance };
                g_screenPose.orientation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
            }
            modlog::Line(std::string("OpenXR: screen mode ") + (g_screenMode ? "ON (flat virtual screen)" : "OFF (immersive)")
                + (manualToggle ? "" : " for the new game's opening"));
            if (manualToggle) PlayCue(g_screenMode ? Cue::On : Cue::Off);
        }
        deleteWasDown = deleteDown;

        // The pose the game actually rendered the presented image with (the
        // camera write from `latency` frames ago). Declaring that -- not the
        // live pose -- is what lets the compositor timewarp the difference
        // away instead of the world swimming behind your head.
        bool headTrackingOn = IsHeadTrackingEnabled();
        HeadSample renderedWith{};
        bool haveRenderedWith = headTrackingOn && HeadTrackingGetSampleForSubmit(&renderedWith);

        // Publish the live pose for the game's next camera writes to use.
        if (poseValid) {
            const XrQuaternionf& o = views[0].pose.orientation;
            const float orient[4] = { o.x, o.y, o.z, o.w };
            const float p0[3] = { views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z };
            const float p1[3] = { views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z };
            HeadTrackingPublish(orient, p0, p1);
            if (AerFrames()) {
                UpdateProjectionMatch(views);
                const XrQuaternionf& a = views[0].pose.orientation;
                const XrQuaternionf& b = views[1].pose.orientation;
                const float dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
                AfwSetEyeViewAngle(2.0f * std::acos((std::min)(1.0f, dot)) * 57.2957795f);
                // Phase 7: the camera writes that use these views are presented ~2 game frames later (Phase 4), so the
                // head pose is predicted for that display time: this frame's + aer_predict_frames display periods.
                const int ahead = GetAerPredictFrames();
                XrTime target = frameState.predictedDisplayTime;
                XrView pv[2]{ {XR_TYPE_VIEW}, {XR_TYPE_VIEW} };
                const XrView* use = views;
                if (ahead > 0) {
                    XrViewLocateInfo li = viewLocateInfo;
                    li.displayTime = frameState.predictedDisplayTime + static_cast<XrTime>(ahead) * frameState.predictedDisplayPeriod;
                    XrViewState vs{XR_TYPE_VIEW_STATE};
                    uint32_t cnt = 0;
                    if (XR_SUCCEEDED(xrLocateViews(g_session, &li, &vs, 2, &cnt, pv)) && cnt == 2
                        && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
                        use = pv;
                        target = li.displayTime;
                    }
                }
                PoseF v[2];
                for (int e = 0; e < 2; ++e) {
                    const XrPosef& p = use[e].pose;
                    v[e] = { { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w }, { p.position.x, p.position.y, p.position.z } };
                }
                AerPublishViews(v, static_cast<int64_t>(target));
            }
        }

        // Alternate-eye rendering: which eye (and pose) the image being presented was rendered with. Only that eye's
        // swapchain is updated; the other keeps its last image and pose. The pairing rule is advanced on every Present
        // (also in menus) so it stays in step; while a menu, map or video is up, or no AER frame is known, the image
        // goes to both eyes as in mono mode.
        int eyeMask = 3;
        bool monoCentre = false;    // Phase 8: both eyes declared with the current head-centre pose (image ignores our writes)
        char fallback = kFbNone;
        if (AerFrames()) {
            // Phase 8: are the camera writes being honoured? Two read-backs:
            //   stalled -- no new game frame for kStallPresents Presents (a real hitch is <= 3 at 72 Hz; the Phase 7
            //              resync covers those), so the frames being presented were not rendered from our writes;
            //   foreign -- a camera write found the struct changed since our previous write (CameraForeignWrites);
            //              held for kForeignHoldPresents so the frames already in the pipeline are covered too.
            // Either way the image does not carry our eye pose: both eyes get the same image and the same pose, the
            // current head centre (flat but comfortable), until writes are honoured again.
            // aer_cutscenes=mono and Ctrl+V instead ask the game to render from the head centre (writes still
            // honoured); those frames go to both eyes with the pose they were rendered with.
            constexpr int kStallPresents = 4;
            constexpr int kForeignHoldPresents = 36;
            static uint64_t lastG = 0, lastForeign = 0;
            static int presentsSinceAdvance = 0, presentsSinceForeign = 1 << 20;
            static char loggedFallback = kFbNone;
            static uint64_t fallbackStart = 0;
            const uint64_t G = CameraLatestGameFrame();
            if (G != lastG) { lastG = G; presentsSinceAdvance = 0; } else if (presentsSinceAdvance < (1 << 20)) ++presentsSinceAdvance;
            const uint64_t fw = CameraForeignWrites();
            if (fw != lastForeign) { lastForeign = fw; presentsSinceForeign = 0; } else if (presentsSinceForeign < (1 << 20)) ++presentsSinceForeign;
            const bool cinematic = CinematicActive();
            const bool cutsceneMono = cinematic && GetAerCutscenesMono();
            AerSetMonoRender(cutsceneMono || g_forceMono);

            g_afwReq = false;
            AerPresented ap{};
            const bool paired = AerPairPresent(G, FrameLogCurrentFrame(), &ap);
            if (!headTrackingOn) fallback = kFbNoTracking;
            else if (MenuScreenActive()) fallback = kFbMenu;
            else if (presentsSinceAdvance >= kStallPresents) fallback = kFbStalled;
            else if (presentsSinceForeign < kForeignHoldPresents) fallback = kFbForeign;
            else if (!paired) fallback = kFbUnpaired;
            else if (g_forceMono) fallback = kFbForced;
            else if (cutsceneMono) fallback = kFbCutscene;
            else if (ap.mono) fallback = kFbMonoFrame;
            else if (ap.corrected) fallback = kFbResync;
            const bool honouredMono = fallback == kFbForced || fallback == kFbCutscene || fallback == kFbMonoFrame || fallback == kFbResync;

            double predErr = std::nan("");
            if (fallback == kFbNone) {
                eyeMask = 1 << ap.eye;
                g_aerEyePose[ap.eye] = ap.pose;
                g_aerEyeFov[ap.eye] = ap.fov;
                g_aerEyeHaveFinal[ap.eye] = ap.haveFinal;
                g_aerEyeFinalFov[ap.eye] = ap.finalFov;
                g_aerEyeFinalAspect[ap.eye] = ap.finalAspect;
                g_aerEyeValid[ap.eye] = true;
                if (!g_aerEyeValid[1 - ap.eye]) {   // first AER frame: fill the other eye too, so it isn't empty
                    eyeMask = 3;
                    g_aerEyePose[1 - ap.eye] = ap.pose;
                    g_aerEyeFov[1 - ap.eye] = ap.fov;
                    g_aerEyeHaveFinal[1 - ap.eye] = ap.haveFinal;
                    g_aerEyeFinalFov[1 - ap.eye] = ap.finalFov;
                    g_aerEyeFinalAspect[1 - ap.eye] = ap.finalAspect;
                    g_aerEyeValid[1 - ap.eye] = true;
                }
                // AFW: the other eye is warped from this frame and declared with its pose at the same moment.
                g_afwReq = false;
                if (g_useD3D12 && !ap.mono && AfwReady(ap.gameFrame)) {
                    const int o = 1 - ap.eye;
                    const float* q = ap.pose.q;   // this eye's right axis: q * (1, 0, 0)
                    const float rx = 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]), ry = 2.0f * (q[0] * q[1] + q[3] * q[2]),
                                rz = 2.0f * (q[0] * q[2] - q[3] * q[1]);
                    const float bxM = (ap.poseOther.p[0] - ap.pose.p[0]) * rx + (ap.poseOther.p[1] - ap.pose.p[1]) * ry
                                    + (ap.poseOther.p[2] - ap.pose.p[2]) * rz;
                    float tanH = 0.0f, tanV = 0.0f;
                    if (ap.haveFinal) { tanV = std::tan(ap.finalFov * 0.5f); tanH = tanV * ap.finalAspect; }
                    else OpenXRPredictFrustum(ap.fov, OpenXRFovModel(), &tanH, &tanV);
                    if (tanH > 0.0f && std::fabs(bxM) > 0.01f && std::fabs(bxM) < 0.2f) {
                        g_afwBaselineCm = bxM * 100.0f * AerWorldScale();
                        g_afwTanH = tanH;
                        g_afwTanV = tanV;
                        g_afwEye = ap.eye;
                        g_afwFrame = ap.gameFrame;
                        g_afwReq = true;
                        eyeMask = 3;
                        g_aerEyePose[o] = ap.poseOther;
                        g_aerEyeFov[o] = ap.fov;
                        g_aerEyeHaveFinal[o] = ap.haveFinal;
                        g_aerEyeFinalFov[o] = ap.finalFov;
                        g_aerEyeFinalAspect[o] = ap.finalAspect;
                        g_aerEyeValid[o] = true;
                    }
                }
                FrameLogSetEyeTag(ap.eye == 0 ? 'L' : 'R');
                const double displayMs = XrTimeToMs(frameState.predictedDisplayTime), targetMs = XrTimeToMs(ap.targetTimeNs);
                if (displayMs > 0.0 && targetMs > 0.0) predErr = displayMs - targetMs;
            } else if (honouredMono) {
                // The image was rendered from our write for frame S: both eyes, with the pose (and FOV) it was rendered
                // with -- one eye's pose (Phase 7 resync, the first frames of a mono request) or the head centre.
                eyeMask = 3;
                for (int e = 0; e < 2; ++e) {
                    g_aerEyePose[e] = ap.pose;
                    g_aerEyeFov[e] = ap.fov;
                    g_aerEyeHaveFinal[e] = ap.haveFinal;
                    g_aerEyeFinalFov[e] = ap.finalFov;
                    g_aerEyeFinalAspect[e] = ap.finalAspect;
                    g_aerEyeValid[e] = true;
                }
                FrameLogSetEyeTag('M');
                if (fallback == kFbResync) ++g_aerStats.resyncs;
            } else {
                g_aerEyeValid[0] = g_aerEyeValid[1] = false;
                monoCentre = true;
                FrameLogSetEyeTag('M');
            }

            // Log the fallbacks that matter for Phase 8 (not menus, resyncs or the head-centre pipeline tail).
            const bool significant = fallback == kFbStalled || fallback == kFbForeign || fallback == kFbUnpaired
                || fallback == kFbCutscene || fallback == kFbForced;
            const uint64_t presentNow = FrameLogCurrentFrame();
            if (significant && fallback != loggedFallback) {
                modlog::Line(std::string("AER: mono fallback -- ") + FallbackName(fallback) + " (Present " + std::to_string(presentNow)
                    + ", game frame " + std::to_string(G) + (cinematic ? ", in a cutscene" : "") + ")");
                if (loggedFallback == kFbNone) fallbackStart = presentNow;
                loggedFallback = fallback;
            } else if (fallback == kFbNone && loggedFallback != kFbNone) {
                modlog::Line(std::string("AER: stereo resumed after ") + std::to_string(presentNow - fallbackStart) + " Presents (last reason: "
                    + FallbackName(loggedFallback) + (cinematic ? "; in a cutscene" : "") + ")");
                loggedFallback = kFbNone;
            }

            FrameLogAer((fallback == kFbNone || honouredMono) ? ap.gameFrame : 0, predErr, g_lastWaitFrameMs);
            AerStatsAdd(fallback == kFbNone, fallback == kFbNone ? ap.eye : -1, ap.gameFrame, predErr, g_lastWaitFrameMs, fallback, cinematic);
        }

        if (g_useD3D12) {
            StallWatchdogSetPhase("xr:copy+warp+hud");
            CopyBackbufferToEyes12(backbuffer12, eyeMask);
            StallWatchdogSetPhase("xr:after-copy");
        } else {
            ID3D11Device* device = nullptr;
            leftEyeTexture->GetDevice(&device);
            ID3D11DeviceContext* context = nullptr;
            if (device) {
                device->GetImmediateContext(&context);
            }

            if (context) {
                CopyIntoSwapchain(g_swapchainLeft, g_swapchainImagesLeft, g_swapEyes ? rightEyeTexture : leftEyeTexture, context);
                CopyIntoSwapchain(g_swapchainRight, g_swapchainImagesRight, g_swapEyes ? leftEyeTexture : rightEyeTexture, context);
                context->Release();
            }
            if (device) {
                device->Release();
            }
        }

        {   // Phase 8 pacing: display step, each eye's image age at display (PACE line, recorder columns)
            float eyeAge[2];
            const int displayStep = PaceAdd(frameState.predictedDisplayTime, frameState.predictedDisplayPeriod, eyeMask,
                                            g_lastWaitFrameMs, eyeAge, headTrackingOn && !MenuScreenActive());
            FrameLogPace(fallback, displayStep, eyeAge[0], eyeAge[1], g_refreshHz);
        }

        for (int eye = 0; eye < 2; ++eye) {
            projViews[eye].pose = views[eye].pose;
            if (monoCentre) {
                // Phase 8 fallback (writes not honoured, menus): the same pose for both eyes -- the head centre, the
                // eyes' midpoint and mean orientation -- so the one image shows no false disparity.
                const XrPosef& l = views[0].pose;
                const XrPosef& r = views[1].pose;
                const float sg = (l.orientation.x * r.orientation.x + l.orientation.y * r.orientation.y
                    + l.orientation.z * r.orientation.z + l.orientation.w * r.orientation.w) < 0.0f ? -1.0f : 1.0f;
                XrQuaternionf q{ l.orientation.x + sg * r.orientation.x, l.orientation.y + sg * r.orientation.y,
                                 l.orientation.z + sg * r.orientation.z, l.orientation.w + sg * r.orientation.w };
                const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
                if (n > 0.0f) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; } else { q = l.orientation; }
                projViews[eye].pose.orientation = q;
                projViews[eye].pose.position = { 0.5f * (l.position.x + r.position.x), 0.5f * (l.position.y + r.position.y),
                                                 0.5f * (l.position.z + r.position.z) };
            } else if (AerFrames() && g_aerEyeValid[eye]) {
                // AER: the eye's own pose -- orientation AND position -- that its current image was rendered with
                // (the game camera now follows head translation too, so the compositor may correct it).
                const PoseF& a = g_aerEyePose[eye];
                projViews[eye].pose.orientation = { a.q[0], a.q[1], a.q[2], a.q[3] };
                projViews[eye].pose.position = { a.p[0], a.p[1], a.p[2] };
            } else if (haveRenderedWith && !AerFrames()) {   // SBS/mono head tracking's own ring (never used under AER)
                projViews[eye].pose.orientation = { renderedWith.q[0], renderedWith.q[1], renderedWith.q[2], renderedWith.q[3] };
                // Position stays the runtime's current eye position: the game camera does not
                // follow head translation, so the image carries no positional information and
                // declaring an old head position would make the compositor "correct" for head
                // translation (neck motion) that the image never had. Only rotation is corrected.
            }

            float renderFov = headTrackingOn ? HeadTrackingGetGameFov() : kGameNativeFov;
            const bool aerEye = AerFrames() && g_aerEyeValid[eye] && g_aerEyeFov[eye] > 0.0f;
            if (aerEye) renderFov = g_aerEyeFov[eye];   // Phase 6: the frustum this eye's image was rendered with
            float halfTanH, halfTanV;
            ComputeHalfTans(renderFov, &halfTanH, &halfTanV);
            if (aerEye) {
                // Assert: the declared frustum equals the one the game is being given now (they differ only while the
                // FOV changes, e.g. the runtime's view changed).
                float nowH, nowV;
                ComputeHalfTans(g_pmGameFov, &nowH, &nowV);
                ++g_pmFrames;
                if (std::fabs(nowH - halfTanH) > 1e-4f * nowH || std::fabs(nowV - halfTanV) > 1e-4f * nowV) ++g_pmMismatches;
                if (g_pmFrames % 1800 == 0) {   // per window of 1800 eye-frames (a FOV change -- e.g. the \ key -- causes a few)
                    modlog::Line("PHASE6: declared vs game frustum: " + std::to_string(g_pmMismatches) + " mismatching eye-frames in the last 1800; "
                        + std::to_string(g_pmCorrected) + " declared from the game's final projection where it differed from the model by > 0.5 % (game FOV correction)");
                    g_pmMismatches = 0;
                    g_pmCorrected = 0;
                }
                // Declare the projection the game really used for this image when known: our FOV + the game's own FOV
                // correction (blends after the examine scene of run 8 warped the view for ~10 s), pinned as the model says.
                if (g_aerEyeHaveFinal[eye]) {
                    const float tv = std::tan(0.5f * g_aerEyeFinalFov[eye]);
                    const float th = tv * g_aerEyeFinalAspect[eye];
                    if (std::fabs(th - halfTanH) > 0.005f * halfTanH || std::fabs(tv - halfTanV) > 0.005f * halfTanV) ++g_pmCorrected;
                    halfTanH = th;
                    halfTanV = tv;
                }
            }
            // Menus, the map and loading screens fill the whole view and the lens edge distorts them, so they are
            // shown smaller, centred in the same place (the declared field of view shrinks, the rest stays black).
            if (MenuScreenActive() && !aerEye) {
                halfTanH *= kMenuScreenScale;
                halfTanV *= kMenuScreenScale;
            }
            float aH = std::atan(halfTanH);
            float aV = std::atan(halfTanV);
            // Positive shift moves an image's centre toward the nose:
            // right for the left eye, left for the right eye.
            float shift = aerEye ? 0.0f : (eye == 0 ? 1.0f : -1.0f) * g_convergenceShift;
            projViews[eye].fov.angleLeft = -aH + shift;
            projViews[eye].fov.angleRight = aH + shift;
            projViews[eye].fov.angleUp = aV;
            projViews[eye].fov.angleDown = -aV;
            projViews[eye].subImage.swapchain = (eye == 0) ? g_swapchainLeft : g_swapchainRight;
            projViews[eye].subImage.imageRect.offset = { 0, 0 };
            projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_eyeWidth), static_cast<int32_t>(g_eyeHeight) };
        }

        // Per-frame record: the runtime's FOV vs ours (static edge coverage), and how far the compositor has to
        // rotate each submitted image to the predicted display pose (reprojection -> trailing black bar).
        {
            float rt[2][4], sub[2][4], reproj[2];
            for (int eye = 0; eye < 2; ++eye) {
                rt[eye][0] = views[eye].fov.angleLeft;  rt[eye][1] = views[eye].fov.angleRight;
                rt[eye][2] = views[eye].fov.angleUp;    rt[eye][3] = views[eye].fov.angleDown;
                sub[eye][0] = projViews[eye].fov.angleLeft; sub[eye][1] = projViews[eye].fov.angleRight;
                sub[eye][2] = projViews[eye].fov.angleUp;   sub[eye][3] = projViews[eye].fov.angleDown;
                const XrQuaternionf& a = projViews[eye].pose.orientation;
                const XrQuaternionf& b = views[eye].pose.orientation;
                float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
                if (d > 1.0f) d = 1.0f;
                reproj[eye] = 2.0f * std::acos(d) * 57.29578f;
            }
            const double age = (haveRenderedWith && predictedDisplayMs > 0.0) ? predictedDisplayMs - renderedWith.tMs : std::nan("");
            FrameLogViews(rt, sub, reproj, age);
        }

        if (g_screenMode) {
            const float aspect = FrameWidthInEyes() * static_cast<float>(g_eyeWidth) / static_cast<float>(g_eyeHeight);   // the frame's own shape
            for (int eye = 0; eye < 2; ++eye) {
                XrCompositionLayerQuad& q = quadLayers[eye];
                q.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                q.layerFlags = 0;
                q.space = g_appSpace;
                q.eyeVisibility = (eye == 0) ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                q.subImage = projViews[eye].subImage;
                q.pose = g_screenPose;
                q.size = { g_screenWidth, g_screenWidth / aspect };
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&q));
            }
        } else {
            layer.space = g_appSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
        }

        // In-headset overlay: status for its pages, its requests, and its panel on top.
        {
            OverlayXrInfo info;
            info.runtime = g_runtimeName;
            info.system = g_systemName;
            info.refreshHz = g_refreshHz;
            if (g_refreshExt) info.rates = g_refreshRates;
            info.eyeW = g_eyeWidth;
            info.eyeH = g_eyeHeight;
            info.screenMode = g_screenMode;
            VrOverlaySetXrInfo(info);
            const float hz = VrOverlayTakeRefreshRequest();
            if (hz > 0.0f) RequestRefresh(hz);
        }
        if (VrOverlayVisible()) {
            int ow = 0, oh = 0;
            StallWatchdogSetPhase("xr:overlay-upload");
            if (const uint8_t* img = VrOverlayTakeImage(&ow, &oh)) OverlayUpload(img, ow, oh);
            if (VrOverlayTakePlace() || !g_ov.placed) OverlayPlace(views);
            if (g_ov.haveImage) {
                overlayQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                overlayQuad.space = g_appSpace;
                overlayQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                overlayQuad.subImage.swapchain = g_ov.sc;
                overlayQuad.subImage.imageRect.offset = { 0, 0 };
                overlayQuad.subImage.imageRect.extent = { g_ov.w, g_ov.h };
                overlayQuad.subImage.imageArrayIndex = 0;
                overlayQuad.pose = g_ov.pose;
                const float wm = VrOverlayWidthM();
                overlayQuad.size = { wm, wm * static_cast<float>(g_ov.h) / static_cast<float>(g_ov.w) };
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&overlayQuad));
            }
        }
    }

    XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
    frameEndInfo.displayTime = frameState.predictedDisplayTime;
    frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    frameEndInfo.layerCount = static_cast<uint32_t>(layers.size());
    frameEndInfo.layers = layers.data();
    StallWatchdogSetPhase("xr:xrEndFrame");
    const double endFrameStart = NowMs();
    XrCheck(xrEndFrame(g_session, &frameEndInfo), "xrEndFrame");
    g_xrEndFrame.Add(NowMs() - endFrameStart);

    if (g_convertTime) {
        FrameRec& rec = g_frameRing[g_frameRingPos];
        g_frameRingPos = (g_frameRingPos + 1) % 8;
        rec.waitReturnMs = waitReturnMs;
        rec.endCallMs = endFrameStart;
        rec.displayMs = XrTimeToMs(frameState.predictedDisplayTime);
        rec.periodMs = static_cast<double>(frameState.predictedDisplayPeriod) / 1.0e6;
        rec.shouldRender = frameState.shouldRender != 0;
        // The display slot advancing by more than one period between consecutive
        // frames = the runtime scheduled our frame a slot late.
        if (!g_halfRate && g_lastDisplayMs > 0.0 && rec.displayMs - g_lastDisplayMs > 1.5 * rec.periodMs) {
            // Compact one-line record of every skip (cadence analysis): seconds since
            // the first skip, ms since the previous skip, and how many slots were lost.
            static double firstSkipMs = 0.0, lastSkipMs = 0.0;
            static int compactLogged = 0;
            const double nowMs = NowMs();
            if (firstSkipMs == 0.0) firstSkipMs = nowMs;
            ++g_skipsInWindow;
            g_slotsLostInWindow += static_cast<int>((rec.displayMs - g_lastDisplayMs) / rec.periodMs - 1.0 + 0.5);
            if (compactLogged < 40) {
                ++compactLogged;
                std::ostringstream c;
                c.setf(std::ios::fixed);
                c.precision(1);
                c << "SKIP t=" << (nowMs - firstSkipMs) / 1000.0 << "s dt=" << (lastSkipMs > 0.0 ? nowMs - lastSkipMs : 0.0)
                  << "ms lost=" << ((rec.displayMs - g_lastDisplayMs) / rec.periodMs - 1.0) << " slots";
                modlog::Line(c.str());
            }
            lastSkipMs = nowMs;
        }
        if (!g_halfRate && g_lastDisplayMs > 0.0 && rec.displayMs - g_lastDisplayMs > 1.5 * rec.periodMs && g_skipEventsLogged < 6) {
            ++g_skipEventsLogged;
            std::ostringstream o;
            o.setf(std::ios::fixed);
            o.precision(2);
            o << "SLOTSKIP #" << g_skipEventsLogged << ": display time advanced " << (rec.displayMs - g_lastDisplayMs)
              << "ms (period " << rec.periodMs << "). Last frames, oldest first [appTime=end-waitReturn, slack=display-end, lead=display-waitReturn]:";
            for (int k = 1; k <= 6; ++k) {
                const FrameRec& r = g_frameRing[(g_frameRingPos + 8 - 7 + k - 1 + 8) % 8];
                o << " {app " << (r.endCallMs - r.waitReturnMs) << " slack " << (r.displayMs - r.endCallMs)
                  << " lead " << (r.displayMs - r.waitReturnMs) << (r.shouldRender ? "" : " NOSHOW") << "}";
            }
            modlog::Line(o.str());
        }
        g_lastDisplayMs = rec.displayMs;
    }

    // Half-rate mode (End toggles): the game only produces every other slot; the
    // in-between slot re-submits the same layer (same rendered pose and images) and
    // the compositor timewarps it to the new head pose, exactly as when the runtime
    // halves a slow app -- but locked, so the game never alternates 72/36.
    static bool insertWasDown = false;
    const bool insertDown = (VRMOD_DEV_KEY(VK_END) & 0x8000) != 0;
    if (insertDown && !insertWasDown && AerFrames()) {
        modlog::Line("OpenXR: half-rate mode (End) is not available with alternate-eye rendering.");
    } else if (insertDown && !insertWasDown) {
        g_halfRate = !g_halfRate;
        modlog::Line(std::string("OpenXR: half-rate mode ") + (g_halfRate ? "ON (game at half the display rate)" : "OFF"));
        PlayCue(g_halfRate ? Cue::On : Cue::Off);
    }
    insertWasDown = insertDown;
    if (g_halfRate && !layers.empty()) {
        XrFrameState extraState{XR_TYPE_FRAME_STATE};
        double extraReturnMs = 0.0;
        if (waitAndBegin(&extraState, &extraReturnMs)) {
            XrFrameEndInfo extraEnd = frameEndInfo;
            extraEnd.displayTime = extraState.predictedDisplayTime;
            XrCheck(xrEndFrame(g_session, &extraEnd), "xrEndFrame(repeat)");
        }
    }

    if (g_pacingEarly) {
        // Start the next frame now; this is where the pacing block happens.
        earlyFrameBegun = false;
        if (waitAndBegin(&earlyFrameState, &earlyWaitReturnMs)) {
            earlyFrameBegun = true;
        }
    }

    static unsigned long long xrFrames = 0;
    if (++xrFrames % 300 == 0) {
        if (GetLogSwitches().perf) modlog::Line("XRPERF (per 300 frames, avg/max): waitFrame " + g_xrWaitFrame.Summary()
            + " waitImage " + g_xrWaitImage.Summary() + " throttle " + g_xrThrottle.Summary()
            + " endFrame " + g_xrEndFrame.Summary()
            + " | slot skips " + std::to_string(g_skipsInWindow) + " (slots lost " + std::to_string(g_slotsLostInWindow) + ")");
        g_skipsInWindow = 0; g_slotsLostInWindow = 0;
        g_xrWaitFrame.Reset(); g_xrWaitImage.Reset(); g_xrThrottle.Reset(); g_xrEndFrame.Reset();
    }
}

} // namespace

namespace {

// F4 in DX12: read the presented backbuffer back to the CPU and write
//   dx12_eye_left.bmp / dx12_eye_right.bmp   (the two halves, exactly what is sent to the headset)
//   dx12_crop_left.bmp                       (1:1 crop from the middle of the left eye, for judging sharpness)
// next to the exe. Runs on the game's queue in the Present hook, so the backbuffer is
// in the PRESENT state and holds the finished (post-DLSS, post-HUD) frame.
// Reads the whole backbuffer back to a tightly packed CPU copy (8-bit 4-channel only).
bool ReadbackBackbuffer12(ID3D12Resource* backbuffer, std::vector<uint8_t>* out, uint32_t* outW, uint32_t* outH) {
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
        && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes = 0;
    g_device12->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = totalBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = SUCCEEDED(g_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))
        && SUCCEEDED(g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
        && SUCCEEDED(g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))
        && SUCCEEDED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))
        && evt;
    if (ok) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = backbuffer;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = backbuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = { list };
        g_gameQueue->ExecuteCommandLists(1, lists);
        g_gameQueue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, evt);
        ok = WaitForSingleObject(evt, 10000) == WAIT_OBJECT_0;
    }
    if (ok) {
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalBytes) };
        if (SUCCEEDED(readback->Map(0, &readRange, &mapped)) && mapped) {
            const uint32_t w = static_cast<uint32_t>(desc.Width), h = desc.Height, pitch = footprint.Footprint.RowPitch;
            out->resize(static_cast<size_t>(w) * h * 4);
            for (uint32_t y = 0; y < h; ++y) {
                memcpy(out->data() + static_cast<size_t>(y) * w * 4, static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * pitch, static_cast<size_t>(w) * 4);
            }
            *outW = w;
            *outH = h;
            D3D12_RANGE none{ 0, 0 };
            readback->Unmap(0, &none);
        } else {
            ok = false;
        }
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (readback) readback->Release();
    if (evt) CloseHandle(evt);
    return ok;
}

// F5: are both eyes redrawn every frame? Reads back two CONSECUTIVE presented frames and
// reports how many sampled pixels changed in each half. With the camera still, an eye the
// game did not re-render this frame is bit-identical to the previous frame.
void CompareConsecutiveFrames12(ID3D12Resource* backbuffer) {
    static bool wasDown = false;
    static bool haveFirst = false;
    static std::vector<uint8_t> first;
    static uint32_t fw = 0, fh = 0;
    const bool isDown = (VRMOD_DEV_KEY(VK_F5) & 0x8000) != 0;
    const bool pressed = isDown && !wasDown;
    wasDown = isDown;
    if (!g_device12 || !g_gameQueue) {
        return;
    }
    if (pressed && !haveFirst) {
        haveFirst = ReadbackBackbuffer12(backbuffer, &first, &fw, &fh);
        if (!haveFirst) modlog::Line("FRAMECMP: first readback failed (backbuffer not 8-bit RGBA/BGRA?).");
        return;
    }
    if (haveFirst) {
        haveFirst = false;
        std::vector<uint8_t> second;
        uint32_t w = 0, h = 0;
        if (!ReadbackBackbuffer12(backbuffer, &second, &w, &h) || w != fw || h != fh) {
            modlog::Line("FRAMECMP: second readback failed.");
            return;
        }
        const uint32_t half = w / 2;
        unsigned long long changed[2] = {}, sampled[2] = {};
        double sumAbs[2] = {};
        for (uint32_t y = 0; y < h; y += 8) {
            for (uint32_t x = 0; x < w; x += 8) {
                const int side = x < half ? 0 : 1;
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                int d = 0;
                for (int c = 0; c < 3; ++c) d = (std::max)(d, std::abs(int(first[i + c]) - int(second[i + c])));
                ++sampled[side];
                sumAbs[side] += d;
                if (d > 2) ++changed[side];
            }
        }
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << "FRAMECMP (consecutive frames; ~0% = that half was NOT redrawn): left half changed "
          << 100.0 * changed[0] / (std::max)(1ull, sampled[0]) << "% (mean diff " << sumAbs[0] / (std::max)(1ull, sampled[0])
          << "), right half changed " << 100.0 * changed[1] / (std::max)(1ull, sampled[1]) << "% (mean diff "
          << sumAbs[1] / (std::max)(1ull, sampled[1]) << ")";
        modlog::Line(o.str());
    }
}

// ` (backtick/tilde key): for 240 consecutive Presents, hash a 128x128 patch from the middle of each
// eye's half of the backbuffer and log which half changed each frame. Cheap enough (~0.5 ms
// per frame) not to disturb the cadence being measured. Pattern characters per Present:
//   B both halves changed   L only left   R only right   . neither (the presented frame was a repeat)
void CadenceProbe12(ID3D12Resource* backbuffer) {
    static bool wasDown = false;
    static bool active = false;
    static int count = 0;
    static std::string pattern;
    static unsigned long long prevL = 0, prevR = 0;
    static int changedFrames = 0, leftOnly = 0, rightOnly = 0, both = 0, none = 0;
    static double startMs = 0.0;
    static ID3D12Resource* rb = nullptr;
    static ID3D12CommandAllocator* alloc = nullptr;
    static ID3D12GraphicsCommandList* list = nullptr;
    static ID3D12Fence* fence = nullptr;
    static HANDLE evt = nullptr;
    static UINT64 fenceValue = 0;

    // Backtick is VK_OEM_3 on a US layout but VK_OEM_8 on a UK one; accept both (plus F6-free alternative: Pause).
    const bool isDown = (VRMOD_DEV_KEY(VK_OEM_3) & 0x8000) != 0 || (VRMOD_DEV_KEY(VK_OEM_8) & 0x8000) != 0
        || (VRMOD_DEV_KEY(VK_PAUSE) & 0x8000) != 0;
    const bool pressed = isDown && !wasDown;
    wasDown = isDown;
    if (!g_device12 || !g_gameQueue) {
        return;
    }
    if (pressed && !active) {
        active = true;
        count = 0;
        pattern.clear();
        prevL = prevR = 0;
        changedFrames = leftOnly = rightOnly = both = none = 0;
        startMs = NowMs();
        modlog::Line("CADENCE: capturing 240 consecutive Presents...");
    }
    if (!active) {
        return;
    }

    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    constexpr UINT kPatch = 128, kPitch = kPatch * 4, kRegionBytes = kPitch * kPatch;   // 512-byte pitch, 64 KiB region
    if (!rb) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kRegionBytes * 2;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(g_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))
            || FAILED(g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
            || FAILED(g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))
            || FAILED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            modlog::Line("CADENCE: could not create probe resources.");
            active = false;
            return;
        }
        list->Close();
    }

    const uint32_t half = static_cast<uint32_t>(desc.Width / 2);
    alloc->Reset();
    list->Reset(alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = backbuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    for (int side = 0; side < 2; ++side) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = static_cast<UINT64>(side) * kRegionBytes;
        dst.PlacedFootprint.Footprint = { desc.Format, kPatch, kPatch, 1, kPitch };
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = backbuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const UINT x0 = static_cast<UINT>(side) * half + half / 2 - kPatch / 2;
        const UINT y0 = desc.Height / 2 - kPatch / 2;
        D3D12_BOX box{ x0, y0, 0, x0 + kPatch, y0 + kPatch, 1 };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = { list };
    g_gameQueue->ExecuteCommandLists(1, lists);
    g_gameQueue->Signal(fence, ++fenceValue);
    fence->SetEventOnCompletion(fenceValue, evt);
    if (WaitForSingleObject(evt, 2000) != WAIT_OBJECT_0) {
        modlog::Line("CADENCE: readback timed out.");
        active = false;
        return;
    }

    unsigned long long hash[2] = { 1469598103934665603ull, 1469598103934665603ull };
    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kRegionBytes * 2 };
    if (SUCCEEDED(rb->Map(0, &readRange, &mapped)) && mapped) {
        const uint8_t* p = static_cast<const uint8_t*>(mapped);
        for (int side = 0; side < 2; ++side) {
            for (UINT i = 0; i < kRegionBytes; ++i) {
                hash[side] = (hash[side] ^ p[side * kRegionBytes + i]) * 1099511628211ull;
            }
        }
        D3D12_RANGE none0{ 0, 0 };
        rb->Unmap(0, &none0);
    }

    if (count > 0) {
        const bool cl = hash[0] != prevL, cr = hash[1] != prevR;
        pattern += cl ? (cr ? 'B' : 'L') : (cr ? 'R' : '.');
        if (cl && cr) ++both; else if (cl) ++leftOnly; else if (cr) ++rightOnly; else ++none;
        if (cl || cr) ++changedFrames;
    }
    prevL = hash[0];
    prevR = hash[1];
    if (++count >= 240) {
        active = false;
        const double secs = (NowMs() - startMs) / 1000.0;
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << "CADENCE: 240 Presents in " << secs << " s (" << 240.0 / secs << "/s); frames with new content: "
          << changedFrames << " (" << changedFrames / secs << "/s) | both " << both << ", left-only " << leftOnly
          << ", right-only " << rightOnly << ", repeats " << none << " | pattern (first 100): " << pattern.substr(0, 100);
        modlog::Line(o.str());
    }
}

// Ctrl+L: measure the head-tracking latency instead of guessing it. For ~120 real frames
// the game camera gets a tiny pseudo-random yaw per frame (see head_tracking.cpp); this probe
// reads a 384x128 patch from the middle of the left eye every Present, measures how far the
// image moved horizontally since the previous Present, and correlates those shifts with the
// injected yaw steps at every candidate delay 0..8. The delay with the strongest match is the
// number of Presents between the camera write and the frame showing it = the latency setting.
// Hold your head still and look at a detailed, static scene.
void LatencyCalProbe12(ID3D12Resource* backbuffer) {
    constexpr UINT kW = 384, kH = 128, kPitch = kW * 4, kBytes = kPitch * kH;
    constexpr int kMaxShift = 64;
    constexpr int kExtraFrames = 10;
    static bool lWasDown = false;
    static bool running = false;
    static std::vector<std::vector<float>> profiles;
    static std::vector<float> shifts;
    static ID3D12Resource* rb = nullptr;
    static ID3D12CommandAllocator* alloc = nullptr;
    static ID3D12GraphicsCommandList* list = nullptr;
    static ID3D12Fence* fence = nullptr;
    static HANDLE evt = nullptr;
    static UINT64 fenceValue = 0;

    // Ctrl+L only (with Alt held it's another key -- it fired with the HUD layer's old Ctrl+Alt+L, ending in the failure tone).
    const bool lDown = (VRMOD_DEV_KEY('L') & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
                       && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
    const bool pressed = lDown && !lWasDown;
    lWasDown = lDown;
    if (!g_device12 || !g_gameQueue) {
        return;
    }
    if (pressed && !running && !HeadTrackingCalibrating()) {
        HeadTrackingStartLatencyCalibration(120);
        running = true;
        profiles.clear();
        shifts.clear();
    }
    if (!running) {
        return;
    }

    const unsigned long long start = HeadTrackingCalStartFrame();
    const int frames = HeadTrackingCalFrames();
    const unsigned long long P = HeadTrackingGetPresentFrame();
    if (P < start) {
        return;   // still waiting for the window to open
    }
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
        && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        modlog::Line("LATCAL: backbuffer isn't 8-bit RGBA/BGRA; can't measure.");
        HeadTrackingFinishCalibration();
        running = false;
        return;
    }
    if (!rb) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kBytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(g_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))
            || FAILED(g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
            || FAILED(g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))
            || FAILED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            modlog::Line("LATCAL: could not create probe resources.");
            HeadTrackingFinishCalibration();
            running = false;
            return;
        }
        list->Close();
    }

    const uint32_t half = static_cast<uint32_t>(desc.Width / 2);
    alloc->Reset();
    list->Reset(alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = backbuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { desc.Format, kW, kH, 1, kPitch };
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = backbuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const UINT x0 = half / 2 - kW / 2, y0 = desc.Height / 2 - kH / 2;
    D3D12_BOX box{ x0, y0, 0, x0 + kW, y0 + kH, 1 };
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = { list };
    g_gameQueue->ExecuteCommandLists(1, lists);
    g_gameQueue->Signal(fence, ++fenceValue);
    fence->SetEventOnCompletion(fenceValue, evt);
    if (WaitForSingleObject(evt, 2000) != WAIT_OBJECT_0) {
        modlog::Line("LATCAL: readback timed out.");
        HeadTrackingFinishCalibration();
        running = false;
        return;
    }

    std::vector<float> profile(kW, 0.0f);
    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, kBytes };
    if (SUCCEEDED(rb->Map(0, &readRange, &mapped)) && mapped) {
        const uint8_t* p = static_cast<const uint8_t*>(mapped);
        for (UINT y = 0; y < kH; ++y) {
            for (UINT x = 0; x < kW; ++x) {
                const uint8_t* px = p + y * kPitch + x * 4;
                profile[x] += 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
            }
        }
        D3D12_RANGE none0{ 0, 0 };
        rb->Unmap(0, &none0);
    }

    // Horizontal shift of this Present's image relative to the previous one, by a 1-D SAD search
    // over the column-brightness profiles, refined to sub-pixel with a parabola.
    float shift = 0.0f;
    if (!profiles.empty()) {
        const std::vector<float>& prev = profiles.back();
        float best = 1e30f, costs[2 * kMaxShift + 1];
        int bestS = 0;
        for (int s = -kMaxShift; s <= kMaxShift; ++s) {
            double sum = 0.0;
            int n = 0;
            for (int x = kMaxShift; x < static_cast<int>(kW) - kMaxShift; ++x) {
                sum += std::fabs(profile[x] - prev[x - s]);
                ++n;
            }
            const float c = static_cast<float>(sum / (std::max)(1, n));
            costs[s + kMaxShift] = c;
            if (c < best) { best = c; bestS = s; }
        }
        shift = static_cast<float>(bestS);
        if (bestS > -kMaxShift && bestS < kMaxShift) {
            const float c0 = costs[bestS - 1 + kMaxShift], c1 = costs[bestS + kMaxShift], c2 = costs[bestS + 1 + kMaxShift];
            const float denom = c0 - 2.0f * c1 + c2;
            if (denom > 1e-6f) shift += 0.5f * (c0 - c2) / denom;
        }
    }
    profiles.push_back(std::move(profile));
    shifts.push_back(shift);

    if (P >= start + static_cast<unsigned long long>(frames) + kExtraFrames) {
        running = false;
        // The offsets must still be readable while scoring, so the calibration is only
        // finished at the end of this block (finishing first zeroed every offset, which made
        // every score 0.00 in the first version).
        // shifts[i] = image shift between Present (start+i-1) and (start+i).
        double bestAbs = -1.0;
        int bestL = -1;
        std::ostringstream o;
        o << "LATCAL: correlation by candidate delay:";
        double scores[9] = {};
        double dots[9] = {}, nds[9] = {};
        for (int L = 0; L <= 8; ++L) {
            double dot = 0.0, nm = 0.0, nd = 0.0;
            for (size_t i = 1; i < shifts.size(); ++i) {
                const unsigned long long Pi = start + i;
                const double o1 = (Pi >= static_cast<unsigned long long>(L)) ? HeadTrackingCalOffset(Pi - L) : 0.0;
                const double o0 = (Pi >= static_cast<unsigned long long>(L) + 1) ? HeadTrackingCalOffset(Pi - 1 - L) : 0.0;
                const double d = o1 - o0;
                dot += shifts[i] * d;
                nm += static_cast<double>(shifts[i]) * shifts[i];
                nd += d * d;
            }
            dots[L] = dot;
            nds[L] = nd;
            scores[L] = (nm > 0.0 && nd > 0.0) ? dot / std::sqrt(nm * nd) : 0.0;
            o << " " << L << "=" << std::fixed << std::setprecision(2) << scores[L];
            if (std::fabs(scores[L]) > bestAbs) { bestAbs = std::fabs(scores[L]); bestL = L; }
        }
        modlog::Line(o.str());
        {
            double sumAbsShift = 0.0;
            for (float s : shifts) sumAbsShift += std::fabs(s);
            modlog::Line("LATCAL: mean |image shift| per frame = " + std::to_string(sumAbsShift / (std::max)(size_t(1), shifts.size())) + " px");
        }
        // Per-frame delay histogram: with the pixels-per-radian scale k fitted at the best delay, ask for
        // every frame where the image clearly moved which delay predicts that shift best. A steady
        // pipeline puts nearly every frame on one delay; a wobbling one spreads across neighbours.
        if (bestL >= 0 && nds[bestL] > 0.0) {
            const double k = dots[bestL] / nds[bestL];
            const double minShift = 0.35 * std::fabs(k) * 2.0 * 0.0105;
            int hist[9] = {};
            int counted = 0;
            for (size_t i = 1; i < shifts.size(); ++i) {
                if (std::fabs(shifts[i]) < minShift) continue;
                const unsigned long long Pi = start + i;
                double bestErr = 1e30;
                int bestFrameL = -1;
                for (int L = 0; L <= 8; ++L) {
                    const double o1 = (Pi >= static_cast<unsigned long long>(L)) ? HeadTrackingCalOffset(Pi - L) : 0.0;
                    const double o0 = (Pi >= static_cast<unsigned long long>(L) + 1) ? HeadTrackingCalOffset(Pi - 1 - L) : 0.0;
                    const double err = std::fabs(shifts[i] - k * (o1 - o0));
                    if (err < bestErr) { bestErr = err; bestFrameL = L; }
                }
                if (bestFrameL >= 0) { ++hist[bestFrameL]; ++counted; }
            }
            std::ostringstream h;
            h << "LATCAL: per-frame delay histogram over " << counted << " frames with clear motion:";
            for (int L = 0; L <= 8; ++L) h << " " << L << "=" << hist[L];
            modlog::Line(h.str());
        }
        HeadTrackingFinishCalibration();
        if (bestL >= 0 && bestAbs > 0.25) {
            modlog::Line("LATCAL: best delay = " + std::to_string(bestL) + " Presents (confidence " + std::to_string(bestAbs) + ").");
            HeadTrackingApplyMeasuredLatency(bestL);
            PlayCue(Cue::Success);
        } else {
            modlog::Line("LATCAL: no clear match (best " + std::to_string(bestAbs) + "); was the scene static and detailed? Nothing changed.");
            PlayCue(Cue::Failure);
        }
    }
}

void DumpBackbuffer12(ID3D12Resource* backbuffer) {
    GhostTestOnPresent(g_device12, g_gameQueue, backbuffer);
    PairingTestOnPresent(g_device12, g_gameQueue, backbuffer);
    ReaderProbeOnPresent();
    ValueScanOnPresent();
    FacingProbeOnPresent();
    BoneScanOnPresent();
    GearHideOnPresent();
    ReticleWatchOnPresent();
    CamStateProbeOnPresent();
    HeadDriveOnPresent();
    AfwOnPresent();
    HudLayerOnPresent();
    NearProbeOnPresent();
    UiProbeOnPresent();
    AimFinderOnPresent();
    AimWatchOnPresent();
    ResolutionCheckOnPresent();
    DlssProbeOnPresent();
    CounterScanOnPresent();
    ProjectionProbeOnPresent(g_device12, g_gameQueue, backbuffer);
    LatencyCalProbe12(backbuffer);
    CadenceProbe12(backbuffer);
    CompareConsecutiveFrames12(backbuffer);
    static bool wasDown = false;
    const bool isDown = (VRMOD_DEV_KEY(VK_F4) & 0x8000) != 0;
    const bool pressed = isDown && !wasDown;
    wasDown = isDown;
    if (!pressed || !g_device12 || !g_gameQueue) {
        return;
    }

    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    const bool swapRB = (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const bool isBgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    if (!swapRB && !isBgra) {
        modlog::Line("DX12 dump: backbuffer format " + std::to_string(desc.Format) + " isn't 8-bit RGBA/BGRA, not dumping.");
        return;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes = 0;
    g_device12->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = totalBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = SUCCEEDED(g_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))
        && SUCCEEDED(g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
        && SUCCEEDED(g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))
        && SUCCEEDED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))
        && evt;

    if (ok) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = backbuffer;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = backbuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();

        ID3D12CommandList* lists[] = { list };
        g_gameQueue->ExecuteCommandLists(1, lists);
        g_gameQueue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, evt);
        ok = WaitForSingleObject(evt, 10000) == WAIT_OBJECT_0;
    }

    if (ok) {
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalBytes) };
        if (SUCCEEDED(readback->Map(0, &readRange, &mapped)) && mapped) {
            const uint8_t* base = static_cast<const uint8_t*>(mapped);
            const uint32_t pitch = footprint.Footprint.RowPitch;
            const uint32_t width = static_cast<uint32_t>(desc.Width);
            const uint32_t height = desc.Height;
            const uint32_t half = width / 2;
            // Names carry the backbuffer size and time so repeated dumps never overwrite each other.
            SYSTEMTIME st;
            GetLocalTime(&st);
            char stamp[64];
            snprintf(stamp, sizeof(stamp), "dx12_%ux%u_%02d%02d%02d", width, height, st.wHour, st.wMinute, st.wSecond);
            const std::string nameL = std::string(stamp) + "_eye_left.bmp";
            const std::string nameR = std::string(stamp) + "_eye_right.bmp";
            const std::string nameC = std::string(stamp) + "_crop_left.bmp";
            const bool a = WriteBmp32(nameL.c_str(), half, height, base, pitch, swapRB);
            const bool b2 = WriteBmp32(nameR.c_str(), half, height, base + half * 4, pitch, swapRB);
            const uint32_t cw = (std::min)(1600u, half), ch = (std::min)(900u, height);
            const uint32_t cx = (half - cw) / 2, cy = (height - ch) / 2;
            const bool c = WriteBmp32(nameC.c_str(), cw, ch, base + static_cast<size_t>(cy) * pitch + cx * 4, pitch, swapRB);
            modlog::Line(std::string("DX12 dump: backbuffer ") + std::to_string(width) + "x" + std::to_string(height)
                + " format " + std::to_string(desc.Format) + " -> " + nameL + (a ? " ok" : " FAILED")
                + ", " + nameR + (b2 ? " ok" : " FAILED") + ", " + nameC + " (1:1 " + std::to_string(cw) + "x"
                + std::to_string(ch) + ") " + (c ? "ok" : "FAILED"));
            D3D12_RANGE none{ 0, 0 };
            readback->Unmap(0, &none);
        } else {
            modlog::Line("DX12 dump: failed to map the readback buffer.");
        }
    } else {
        modlog::Line("DX12 dump: GPU copy failed or timed out.");
    }

    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (readback) readback->Release();
    if (evt) CloseHandle(evt);
}

} // namespace

void UpdateOpenXR(IDXGISwapChain* swapChain) {
    if (!swapChain || g_shuttingDown.load()) {
        return;
    }

    // Diagnostic: a file named vrmod_noxr.txt next to the exe disables all OpenXR
    // work (and keeps SteamVR from being auto-started), so game/driver resolution
    // behaviour can be tested with SteamVR out of the picture.
    static const bool xrDisabled = [] {
        bool disabled = GetFileAttributesW(L"vrmod_noxr.txt") != INVALID_FILE_ATTRIBUTES;
        if (disabled) {
            modlog::Line("OpenXR: DISABLED by vrmod_noxr.txt (diagnostic mode, SteamVR not used).");
        }
        return disabled;
    }();
    if (xrDisabled) {
        // Diagnostic mode still allows the cadence/dump probes on a D3D12 game.
        static ID3D12Device* diagDevice = nullptr;
        if (!diagDevice) {
            swapChain->GetDevice(IID_PPV_ARGS(&diagDevice));
            g_device12 = diagDevice;
        }
        if (diagDevice && g_gameQueue) {
            IDXGISwapChain3* sc3 = nullptr;
            ID3D12Resource* bb = nullptr;
            if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
                sc3->Release();
            }
            if (bb) {
                DumpBackbuffer12(bb);
                bb->Release();
            }
        }
        return;
    }

    std::lock_guard<std::timed_mutex> lock(g_xrMutex);
    if (g_shuttingDown.load()) {
        return;
    }

    // Which API is the game running? A D3D12 swapchain answers GetDevice() with an
    // ID3D12Device; a D3D11 one with an ID3D11Device.
    static int apiMode = 0;   // 0 unknown, 11, 12
    if (apiMode == 0) {
        ID3D12Device* d12 = nullptr;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&d12))) && d12) {
            apiMode = 12;
            g_device12 = d12;   // keep the reference for the session
            modlog::Line("OpenXR: game is running D3D12.");
        } else {
            apiMode = 11;
            modlog::Line("OpenXR: game is running D3D11.");
        }
    }

    ID3D12Resource* backbuffer12 = nullptr;
    if (apiMode == 12) {
        g_useD3D12 = true;
        if (!g_gameQueue) {
            return;   // the swapchain-creation hook hasn't seen the game's queue (yet)
        }
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
            UINT index = sc3->GetCurrentBackBufferIndex();
            sc3->GetBuffer(index, IID_PPV_ARGS(&backbuffer12));
            sc3->Release();
        }
        if (!backbuffer12) {
            return;
        }
        D3D12DebugDrain(g_device12);
        DumpBackbuffer12(backbuffer12);
    }

    if (!g_initialized && !g_initFailed) {
        if (apiMode == 12) {
            D3D12_RESOURCE_DESC bd = backbuffer12->GetDesc();
            g_backbufferFormat = bd.Format;
            modlog::Line("OpenXR: D3D12 backbuffer " + std::to_string(bd.Width) + "x" + std::to_string(bd.Height)
                + " format " + std::to_string(bd.Format));
            modlog::Line(std::string("OpenXR: render mode ") + RenderModeName(GetRenderMode())
                + (MonoFrames() ? " -- the whole backbuffer goes to both eyes (set the game's Stereoscopic 3D OFF)"
                                : " -- left half to the left eye, right half to the right (game Stereoscopic 3D ON)"));
            InitOpenXR(nullptr, EyeWidthForBackbuffer(bd.Width), bd.Height);
        } else {
            ID3D11Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
                ID3D11Device* device = nullptr;
                backBuffer->GetDevice(&device);
                if (device) {
                    InitOpenXR(device);
                    device->Release();
                }
                backBuffer->Release();
            }
        }
    }

    if (g_initialized) {
        SubmitStereoFrame(GetLeftEyeTexture(), GetRightEyeTexture(), backbuffer12);
        HeadTrackingAdvanceFrame();
    }
    if (backbuffer12) {
        backbuffer12->Release();   // GetBuffer added a reference
    }
}

void OpenXRScreenModeForIntro(bool on) {
    g_screenIntroRequest.store(on ? 1 : 0);
}

void OpenXRNoteSwapChainDevice(IUnknown* deviceOrQueue) {
    if (!deviceOrQueue) {
        return;
    }
    ID3D12CommandQueue* queue = nullptr;
    if (SUCCEEDED(deviceOrQueue->QueryInterface(IID_PPV_ARGS(&queue))) && queue) {
        if (g_gameQueue) {
            g_gameQueue->Release();
        }
        g_gameQueue = queue;   // keeps the QueryInterface reference
        modlog::Line("OpenXR: captured the game's D3D12 command queue.");
    }
}


int OpenXRFovModel() {
    return g_fovModel;
}

void OpenXRPredictFrustum(float fov, int model, float* tanH, float* tanV) {
    const int saved = g_fovModel;
    g_fovModel = model;
    ComputeHalfTans(fov, tanH, tanV);
    g_fovModel = saved;
}

float OpenXRAerGameFov() {
    return g_pmGameFov;
}

XrPipelineConfig OpenXRGetPipelineConfig() {
    XrPipelineConfig c;
    c.framesAhead = g_maxFramesAhead;
    c.halfRate = g_halfRate;
    return c;
}

// Called from the Present thread (the pairing test), the same thread that reads these.
void OpenXRSetPipelineConfig(const XrPipelineConfig& config) {
    g_maxFramesAhead = (std::max)(0, (std::min)(3, config.framesAhead));
    g_halfRate = config.halfRate;
    modlog::Line("OpenXR: pipeline set to framesAhead=" + std::to_string(g_maxFramesAhead)
        + " halfRate=" + (g_halfRate ? "1" : "0"));
}

void ShutdownOpenXR() {
    if (g_shuttingDown.exchange(true)) {
        return;
    }
    modlog::Line("OpenXR: shutdown requested.");

    std::unique_lock<std::timed_mutex> lock(g_xrMutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1500))) {
        modlog::Line("OpenXR: shutdown couldn't take the frame lock (render thread stuck inside an XR call?) -- skipping teardown.");
        return;
    }

    if (!g_initialized) {
        modlog::Line("OpenXR: nothing to tear down.");
        return;
    }
    g_initialized = false;
    g_sessionRunning = false;

    if (g_useD3D12 && g_fence && g_fenceEvent && g_fence->GetCompletedValue() < g_fenceValue) {
        g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 1000);   // our last copy must finish before the images go away
    }
    GpuFrameTimerStop();
    if (g_swapchainLeft != XR_NULL_HANDLE) { xrDestroySwapchain(g_swapchainLeft); g_swapchainLeft = XR_NULL_HANDLE; }
    if (g_swapchainRight != XR_NULL_HANDLE) { xrDestroySwapchain(g_swapchainRight); g_swapchainRight = XR_NULL_HANDLE; }
    if (g_appSpace != XR_NULL_HANDLE) { xrDestroySpace(g_appSpace); g_appSpace = XR_NULL_HANDLE; }
    if (g_session != XR_NULL_HANDLE) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }
    if (g_cmdList) { g_cmdList->Release(); g_cmdList = nullptr; }
    for (auto& a : g_allocators) { if (a) { a->Release(); a = nullptr; } }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_instance != XR_NULL_HANDLE) { xrDestroyInstance(g_instance); g_instance = XR_NULL_HANDLE; }
    modlog::Line("OpenXR: torn down.");
}


} // namespace vrmod
