#include "eye_gaze.h"

#include <windows.h>
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

#include "logger.h"
#include "openvr_gaze.h"
#include "perf_stats.h"

namespace vrmod {

namespace {

XrInstance g_inst = XR_NULL_HANDLE;
XrSession g_sess = XR_NULL_HANDLE;
XrActionSet g_set = XR_NULL_HANDLE;
XrAction g_gaze = XR_NULL_HANDLE;
XrSpace g_gazeSpace = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
bool g_ready = false;

std::mutex g_mutex;
float g_dir[3] = { 0.0f, 0.0f, -1.0f };
double g_sampleMs = 0.0;
bool g_have = false;
uint64_t g_updates = 0, g_valid = 0;
// Diagnostics (every 10 s): where does the gaze stop?
uint64_t g_calls = 0, g_syncFail = 0, g_inactive = 0, g_locateFail = 0, g_untracked = 0;
XrResult g_lastSync = XR_SUCCESS;
XrSpaceLocationFlags g_lastFlags = 0;

bool Check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    modlog::Line(std::string("EyeGaze: ") + what + " failed (" + std::to_string(static_cast<int>(r)) + ").");
    return false;
}

} // namespace

void EyeGazeInitSession(void* instance, void* session, uint64_t systemId) {
    g_inst = static_cast<XrInstance>(instance);
    g_sess = static_cast<XrSession>(session);
    g_ready = false;
    XrSystemEyeGazeInteractionPropertiesEXT eyeProps{ XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT };
    XrSystemProperties props{ XR_TYPE_SYSTEM_PROPERTIES };
    props.next = &eyeProps;
    if (XR_FAILED(xrGetSystemProperties(g_inst, static_cast<XrSystemId>(systemId), &props)) || !eyeProps.supportsEyeGazeInteraction) {
        modlog::Line("EyeGaze: the headset reports no eye gaze; gaze input off.");
        return;
    }
    XrActionSetCreateInfo si{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(si.actionSetName, "vrmod_gaze");
    strcpy_s(si.localizedActionSetName, "VR mod eye gaze");
    if (!Check(xrCreateActionSet(g_inst, &si, &g_set), "xrCreateActionSet")) return;
    XrActionCreateInfo ai{ XR_TYPE_ACTION_CREATE_INFO };
    ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(ai.actionName, "gaze");
    strcpy_s(ai.localizedActionName, "Eye gaze");
    if (!Check(xrCreateAction(g_set, &ai, &g_gaze), "xrCreateAction")) return;
    XrPath profile = XR_NULL_PATH, pose = XR_NULL_PATH;
    xrStringToPath(g_inst, "/interaction_profiles/ext/eye_gaze_interaction", &profile);
    xrStringToPath(g_inst, "/user/eyes_ext/input/gaze_ext/pose", &pose);
    XrActionSuggestedBinding binding{ g_gaze, pose };
    XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = profile;
    sb.suggestedBindings = &binding;
    sb.countSuggestedBindings = 1;
    if (!Check(xrSuggestInteractionProfileBindings(g_inst, &sb), "xrSuggestInteractionProfileBindings")) return;
    XrSessionActionSetsAttachInfo at{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1;
    at.actionSets = &g_set;
    if (!Check(xrAttachSessionActionSets(g_sess, &at), "xrAttachSessionActionSets")) return;
    XrActionSpaceCreateInfo asi{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    asi.action = g_gaze;
    asi.poseInActionSpace.orientation.w = 1.0f;
    if (!Check(xrCreateActionSpace(g_sess, &asi, &g_gazeSpace), "xrCreateActionSpace")) return;
    XrReferenceSpaceCreateInfo vsi{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    vsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vsi.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(g_sess, &vsi, &g_viewSpace), "xrCreateReferenceSpace(VIEW)")) return;
    g_ready = true;
    modlog::Line("EyeGaze: eye gaze action ready (XR_EXT_eye_gaze_interaction).");
}

void EyeGazeUpdate(int64_t predictedDisplayTime) {
    OpenVrGazePoll();   // SteamVR's OpenVR eye tracking, where the OpenXR action gives nothing
    if (!g_ready) return;
    XrActiveActionSet active{ g_set, XR_NULL_PATH };
    XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    ++g_calls;
    {
        static double lastDiag = 0.0;
        if (NowMs() - lastDiag > 10000.0) {
            lastDiag = NowMs();
            XrPath eyes = XR_NULL_PATH;
            xrStringToPath(g_inst, "/user/eyes_ext", &eyes);
            XrInteractionProfileState prof{ XR_TYPE_INTERACTION_PROFILE_STATE };
            char profName[XR_MAX_PATH_LENGTH] = "(none)";
            if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(g_sess, eyes, &prof)) && prof.interactionProfile != XR_NULL_PATH) {
                uint32_t n = 0;
                xrPathToString(g_inst, prof.interactionProfile, sizeof profName, &n, profName);
            }
            char b[320];
            snprintf(b, sizeof(b), "EyeGaze (10 s): %llu frames | sync failed %llu (last result %d) | action inactive %llu | locate failed %llu | "
                     "not tracked %llu (last flags 0x%llx) | tracked %llu | eyes profile: %s",
                     static_cast<unsigned long long>(g_calls), static_cast<unsigned long long>(g_syncFail), static_cast<int>(g_lastSync),
                     static_cast<unsigned long long>(g_inactive), static_cast<unsigned long long>(g_locateFail),
                     static_cast<unsigned long long>(g_untracked), static_cast<unsigned long long>(g_lastFlags),
                     static_cast<unsigned long long>(g_valid), profName);
            modlog::Line(b);
            g_calls = g_syncFail = g_inactive = g_locateFail = g_untracked = 0;
            g_valid = 0;
        }
    }
    g_lastSync = xrSyncActions(g_sess, &sync);
    if (XR_FAILED(g_lastSync) || g_lastSync == XR_SESSION_NOT_FOCUSED) { ++g_syncFail; return; }   // e.g. not focused
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = g_gaze;
    XrActionStatePose st{ XR_TYPE_ACTION_STATE_POSE };
    if (XR_FAILED(xrGetActionStatePose(g_sess, &gi, &st)) || !st.isActive) { ++g_inactive; return; }
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g_gazeSpace, g_viewSpace, predictedDisplayTime, &loc))) { ++g_locateFail; return; }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_updates;
    g_lastFlags = loc.locationFlags;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if ((loc.locationFlags & need) != need) { ++g_untracked; return; }
    // The gaze looks along the pose's -Z axis.
    const XrQuaternionf& q = loc.pose.orientation;
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    g_dir[0] = -(2.0f * (x * z + w * y));
    g_dir[1] = -(2.0f * (y * z - w * x));
    g_dir[2] = -(1.0f - 2.0f * (x * x + y * y));
    g_sampleMs = NowMs();
    g_have = true;
    ++g_valid;
    static double lastLog = 0.0;
    if (NowMs() - lastLog > 30000.0) {
        lastLog = NowMs();
        char b[160];
        snprintf(b, sizeof(b), "EyeGaze (30 s): %llu updates, %llu tracked; latest direction (%.3f, %.3f, %.3f)",
                 static_cast<unsigned long long>(g_updates), static_cast<unsigned long long>(g_valid), g_dir[0], g_dir[1], g_dir[2]);
        modlog::Line(b);
        g_updates = 0;
    }
}

bool EyeGazeLatest(float dir[3], double maxAgeMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_have || NowMs() - g_sampleMs > maxAgeMs) return false;
    for (int k = 0; k < 3; ++k) dir[k] = g_dir[k];
    return true;
}

void EyeGazeShutdown() {
    if (g_gazeSpace) { xrDestroySpace(g_gazeSpace); g_gazeSpace = XR_NULL_HANDLE; }
    if (g_viewSpace) { xrDestroySpace(g_viewSpace); g_viewSpace = XR_NULL_HANDLE; }
    if (g_set) { xrDestroyActionSet(g_set); g_set = XR_NULL_HANDLE; g_gaze = XR_NULL_HANDLE; }
    g_ready = false;
}

} // namespace vrmod
