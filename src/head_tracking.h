#pragma once

namespace vrmod {

// One published headset sample, in OpenXR LOCAL-space axes (x right, y up,
// z back). Both eyes share an orientation on this setup (Pimax parallel
// projection), so a single quaternion (x, y, z, w) is kept.
struct HeadSample {
    float q[4];
    float eyePos[2][3];
    double tMs;   // QPC milliseconds when the sample was published
};

// OpenXR side: called once per frame after xrLocateViews with a valid pose.
// The first call also switches the camera-struct override on.
void HeadTrackingPublish(const float orientation[4], const float eyePos0[3], const float eyePos1[3]);

// Camera-write hook side (game thread, inside the camera update): the
// recentred head rotation expressed in the game camera's local frame
// (X right, Y forward, Z up -- per the IGCS SOTTR reference). Returns false
// if no head pose has been published yet. Also records which published
// sample this game frame used, so the compositor can be told the pose the
// image was actually rendered with.
bool HeadTrackingComputeGameDelta(float outQuat[4]);

// Freeze (ghost test): while on, HeadTrackingComputeGameDelta keeps returning the head rotation from the first
// camera write after it was switched on, so head movement can't move the game camera.
void HeadTrackingFreeze(bool on);

// The head sample the *currently presented* image was rendered with (the
// camera write from `latency` frames ago). Returns false if unknown, in which
// case the caller should fall back to the live pose.
bool HeadTrackingGetSampleForSubmit(HeadSample* out);

// Hotkeys: F8 recentre yaw, '-'/'=' render-to-display latency in frames
// (default 2).
void HeadTrackingPollHotkeys();
// Recentre yaw at the next published pose, as F8 does (any thread).
void HeadTrackingRequestRecenter();

// Call once per Present, after the frame has been submitted.
void HeadTrackingAdvanceFrame();
unsigned long long HeadTrackingGetPresentFrame();

// Vertical FOV (radians) forced into the game camera while head tracking is on.
float HeadTrackingGetGameFov();
void HeadTrackingAdjustFov(float factor);
void HeadTrackingSetFovDefault(float fov);   // startup value from vrmod_settings.ini (game_fov)
void HeadTrackingResetTuning();

// Latency calibration: for `frames` consecutive real frames the camera gets a small
// pseudo-random yaw offset per frame; the presented images' horizontal shifts are then
// correlated with those offsets to find how many frames the pipeline delays them.
void HeadTrackingStartLatencyCalibration(int frames);
bool HeadTrackingCalibrating();
unsigned long long HeadTrackingCalStartFrame();
int HeadTrackingCalFrames();
float HeadTrackingCalOffset(unsigned long long frame);   // radians; 0 outside the calibration window
void HeadTrackingFinishCalibration();
void HeadTrackingApplyMeasuredLatency(int frames);

// Frame-number matching (Ctrl+M): match each presented frame to the camera write of the same frame
// ordinal instead of looking back a fixed number of frames. Independent of pipeline-depth changes.
void HeadTrackingToggleOrdinalMatching();

// Something happened that can throw the frame-number alignment off (cutscene, loading, long stall): the
// fixed latency is used until the next full statistics window re-derives the alignment automatically.
void HeadTrackingMarkDiscontinuity(const char* why);
void HeadTrackingSetOrdinalDefault(bool on);     // startup default (matching switches itself on after ~600 frames)
bool HeadTrackingOrdinalMatchingWanted();

// Latency (real frames) used at startup and restored by the reset key; and the live value.
void HeadTrackingSetLatencyDefault(int frames);
int HeadTrackingGetLatency();

} // namespace vrmod
