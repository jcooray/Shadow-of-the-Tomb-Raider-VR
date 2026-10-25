#pragma once

#include <cstdint>

namespace vrmod {

// Eye tracking through our own OpenXR session (XR_EXT_eye_gaze_interaction, already enabled at instance creation when
// the runtime offers it). EyeGazeInitSession: after xrCreateSession -- one pose action bound to the eye gaze profile,
// attached once, plus a VIEW reference space. EyeGazeUpdate: once per frame -- syncs the action and locates the gaze
// in view space (x right, y up, -z forward). EyeGazeLatest: the latest gaze direction relative to the head, and the
// age of the sample; false if there is none (not supported, not tracking, or older than maxAgeMs).
void EyeGazeInitSession(void* instance, void* session, uint64_t systemId);
void EyeGazeUpdate(int64_t predictedDisplayTime);
bool EyeGazeLatest(float dir[3], double maxAgeMs);
void EyeGazeShutdown();

}
