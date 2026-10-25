#pragma once

#include <cstdint>

namespace vrmod {

// Eye tracking through SteamVR's OpenVR interface, for foveated DLSS (docs/DLSS.md). SteamVR's OpenXR runtime doesn't
// deliver XR_EXT_eye_gaze_interaction for every headset (Pimax on SteamVR: the action stays inactive), but its OpenVR
// interface reports the eye-tracked foveation centre (IVRSystem::GetEyeTrackedFoveationCenter), which is what
// CheekyFoveatedDLSS itself reads in OpenVR games.
//
// Loads SteamVR's own installed openvr_api.dll (runtime path from %LOCALAPPDATA%\openvr\openvrpaths.vrpath), connects
// as a background application (never starts SteamVR, never shuts the connection down -- the process's OpenXR runtime
// shares the same client) and polls once per frame. [gaze] openvr=0 turns it off.
void OpenVrGazePoll();                                       // once per frame (after xrWaitFrame)
// The latest gaze of one eye (0 left, 1 right) as a direction relative to the head (x right, y up, -z forward); false
// if there is none (not connected, not tracking, or older than maxAgeMs). stamp (optional): a counter that advances with
// every new gaze sample (it stays put while the last gaze is held through a blink).
bool OpenVrGazeLatest(int eye, float dir[3], double maxAgeMs, uint64_t* stamp = nullptr);

}
