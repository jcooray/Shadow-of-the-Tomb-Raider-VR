#pragma once

#include <cstdint>

namespace vrmod {

// Eye tracking through our own OpenXR session (XR_EXT_eye_gaze_interaction, already enabled at instance creation when
// the runtime offers it). EyeGazeCreateActions: after xrCreateSession -- one pose action bound to the eye gaze profile;
// returns its action set (XrActionSet), or nullptr without eye gaze. motion_input.cpp attaches it with the controllers'
// set (a session attaches action sets only once), then calls EyeGazeActionsAttached (the gaze and VIEW spaces).
// EyeGazeUpdate: once per frame, after motion_input.cpp's xrSyncActions (its result) -- locates the gaze in view space
// (x right, y up, -z forward). EyeGazeLatest: the latest gaze direction relative to the head, and the age of the sample;
// false if there is none (not supported, not tracking, or older than maxAgeMs).
void* EyeGazeCreateActions(void* instance, void* session, uint64_t systemId);
void EyeGazeActionsAttached();
void EyeGazeUpdate(int64_t predictedDisplayTime, int syncResult);
bool EyeGazeLatest(float dir[3], double maxAgeMs);
void EyeGazeShutdown();

}
