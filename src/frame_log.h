#pragma once

#include <cstdint>
#include <string>

namespace vrmod {

// Phase 1 instrumentation (see docs/PHASE1.md).
//
// Ctrl+J toggles a per-frame recorder. While it runs, every real Present adds one row, and every game write
// to the camera struct adds one camera-write record. Stopping it (or quitting the game) writes two CSVs next to
// the exe: vrmod_frames_<time>.csv and vrmod_camwrites_<time>.csv, plus a _meta.txt.
// Ctrl+U toggles the eye tint (a red block in the left eye's image, green in the right's).
// Ctrl+Y toggles the flip test (camera pitched +60/-60 degrees on alternate game frames).

// The camera struct's pose: position (world units, cm), rotation quaternion x y z w in the game's camera-local
// frame (X right, Y forward, Z up), vertical FOV in radians.
struct CamPose {
    float p[3];
    float q[4];
    float fov;
};

// Game thread, from the camera-write hook, once per game write to the struct:
//   pre  = the struct just before the game's write (the previous write's result, as it survived)
//   game = the struct after the game's own write, before our changes
//   ours = what we leave in the struct
// gameFrame = game frame detected from write timing (a gap > 2 ms since the previous write starts a new one).
void FrameLogCameraWrite(const CamPose& pre, const CamPose& game, const CamPose& ours, int flipSign, uint64_t gameFrame);

// Present thread, in order, once per real Present.
void FrameLogPresentBegin(uint64_t presentFrame, uint64_t xrFrame, const CamPose* structAtPresent);
void FrameLogXrFrame(double waitReturnMs, int64_t predictedDisplayTimeNs, double predictedDisplayMs,
                     double periodMs, bool shouldRender);
void FrameLogViews(const float runtimeFov[2][4], const float submittedFov[2][4], const float reprojDeg[2],
                   double sampleAgeMs);
// Phase 7 (AER): the game frame presented (0 = none / mono), the prediction error (display time - the time its pose was
// predicted for, ms; NaN if unknown) and how long xrWaitFrame blocked for this frame's state (ms).
void FrameLogAer(uint64_t gameFrame, double predErrMs, double xrWaitMs);
// Phase 8: the fallback reason (0 = none; see openxr_integration.cpp Fallback), the display step (periods since the
// previous Present's display; 1 = on time), each eye's image age at display (periods) and the headset refresh (Hz).
void FrameLogPace(char fallback, int displayStep, float eyeAgeL, float eyeAgeR, float refreshHz);
void FrameLogPresentEnd();
uint64_t FrameLogCurrentFrame();   // the Present frame being processed

void FrameLogNotePresent1();                             // any thread
void FrameLogAddDebugMessages(uint32_t count);           // present thread
void FrameLogSetGpuMs(uint64_t presentFrame, float ms);  // GPU timer worker thread
void FrameLogSetMeta(const std::string& key, const std::string& value);

void FrameLogPollHotkeys();   // present thread, once per frame
void FrameLogShutdown();      // flushes synchronously if recording (process exit)

bool FrameLogTintEnabled();

// Flip test pose control, read by the camera-write hook each write:
//   Off, Alternate (+60 / -60 degrees pitch on alternate game frames; Ctrl+Y), HoldPlus / HoldMinus (+60 or -60 on
//   every frame). The ghost test (Ctrl+G) forces a mode while it runs; otherwise Ctrl+Y decides.
//   Random (pairing test, Ctrl+P): +60 or -60 by a pseudo-random bit of the game frame, so the pose a presented
//   frame shows identifies which game frame's write it came from (strict alternation only gives that modulo 2).
enum class FlipMode { Off = 0, Alternate = 1, HoldPlus = 2, HoldMinus = 3, Random = 4 };
FlipMode FrameLogFlipMode();
void FrameLogForceFlipMode(FlipMode mode);   // Off = release (back to the Ctrl+Y toggle)
void FrameLogSetFlipSeed(uint64_t seed);
int FrameLogRandomFlipSign(uint64_t gameFrame);   // +1 / -1, the same answer every time for a given frame and seed

// Recorder control for automated tests (same as Ctrl+J). Stop writes the CSVs on a background thread.
void FrameLogStartRecording();
void FrameLogStopRecording();
bool FrameLogIsRecording();

// Per-row eye tag: 'S' (both eyes from one SBS frame), 'M' (mono frame to both eyes). Later: 'L' / 'R'.
void FrameLogSetEyeTag(char tag);

// The game's active settings block, as last printed to its own log (Documents\Shadow of the Tomb Raider\...log).
std::string FrameLogReadGameSettings();

}
