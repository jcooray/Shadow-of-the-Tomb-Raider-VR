#pragma once

#include <cstdint>

namespace vrmod {

// Finds and hooks the real player camera struct, using the AOB pattern and
// struct offsets published in FransBouma/InjectableGenericCameraSystem's
// SOTTR camera implementation (BSD-style licensed, see camera_interceptor.asm
// for the source reference). Confirmed independently this session via a live
// Cheat Engine breakpoint that RDX holds this exact pointer at this exact
// instruction.
//
// Struct layout (from GameConstants.h in the reference project):
//   +0x80  Vec3 position (3 floats)
//   +0xA0  Quaternion rotation (4 floats, x y z w)
//   +0xB0  float FOV
bool InstallCameraInterceptor();

// Tap F9 in-game to log the live position/rotation/FOV, same convention as
// the rest of the mod's diagnostic hooks.
void LogCameraStructIfHotkeyDown();

// Tap F11 in-game to toggle the head-tracking camera override on/off (it's
// switched on automatically once the first headset pose is published).
void TestCameraWriteIfHotkeyPressed();

// Switches the camera write hook's head-tracking patch on/off.
void SetHeadTrackingEnabled(bool enabled);
bool IsHeadTrackingEnabled();

// F6 arms a trace of the next 128 camera writes (pre-patch values as the game
// wrote them: position, rotation, FOV, timing, which Present interval).
// Call both once per frame from hkPresent; the flush logs CAMTRACE lines.
void PollCameraTraceHotkey();

// First-person proof of concept: Home toggles, PageUp/PageDown = forward distance,
// Shift+PageUp/PageDown = height. Call once per frame.
void PollFirstPersonHotkeys();

// True while a menu, the map, a loading screen or a video is up (head tracking is off for it).
bool MenuScreenActive();

// The game's own state as last polled (once per frame): playing-state (5 = gameplay), menus open, video
// playing. -1 = could not be read.
void GameUiSnapshot(int* state, int* menuOpen, int* movie);

// Instrumentation: total game writes (by the hooked copy) to a struct other than ours, and our struct's address.
uint64_t CameraOtherStructWrites();
// Game frame of the latest write to our camera struct (detected from write timing; see ApplyCameraHeadTracking).
uint64_t CameraLatestGameFrame();

// D2 (docs/DLSS.md): the final camera written for a game frame (after head, eye offset and first person): world
// position (cm), local->world rotation (X right, Y forward, Z up), and the near / far planes (cm). False if the frame
// is no longer in the ring (the last 16 frames are kept).
struct CamFrameRec { uint64_t frame; float pos[3]; float q[4]; float nearCm, farCm; };
bool CameraFrameRecord(uint64_t frame, CamFrameRec* out);
// Split frames merged so far: one game frame's two writes more than 1 ms apart (same game pose; see
// ApplyCameraHeadTracking).
uint64_t CameraSplitMerges();
// Phase 8: camera writes where the struct no longer held what we left after the previous write (something else wrote
// the camera in between, so our pose is not what gets rendered).
uint64_t CameraForeignWrites();
bool IsFirstPersonEnabled();
// In-headset overlay (vr_overlay.cpp): first person switched as by the gamepad combo (on the next Present); head aim
// (aer only) and head hiding read and set.
void RequestFirstPersonToggle();
bool HeadAimEnabled();
void SetHeadAimEnabled(bool on, const char* how);
bool HideHeadEnabled();
void SetHideHeadEnabled(bool on, const char* how);
// Lara is aiming: the camera component's blend stacks use an AimCameraController (camera-mode watch, Present thread).
bool CameraAimingNow();
// Game frames so far in which a player-controlled camera (exploration / aim / stealth) was the base camera.
uint64_t PlayerCameraFrames();
// The rope-shot camera (a Simple base camera right after the aim camera, while a rope arrow charges) is in use.
bool CameraRopeShot();
// Writes where the game only adjusted our camera (rotation within 1 degree, position within 10 cm; stereo survives),
// and the largest such position change since the previous call (cm; resets).
uint64_t CameraAdjustedWrites();
// Aim work (docs/AIM.md): the game's own camera rotation of the latest write (x y z w, before our head delta), and the
// gameplay camera object (FUN_14049a220's first parameter, captured at the camera read hook).
void CameraGameQuat(float out[4]);
uintptr_t CameraOwnerAddress();
float CameraTakeAdjustMaxCm();
uint32_t CameraWriteThreadId();   // the thread that writes the camera struct (0 until the first write)
// While on, each camera write stores (game frame & 63) in FOV mantissa bits 2..7 (low 8 bits = tag << 2; <= 252 ulp,
// ~4e-5 rad).
void CameraSetFovTagging(bool on);
// Diagnostic: the next camera write sleeps `ms` on the game thread (once), making that game frame late.
void CameraInjectStall(int ms);
uintptr_t CameraStructAddress();

// The camera struct's current pose, read under SEH (safe from any thread). False if unknown/unreadable.
struct CamPose;
bool CameraReadStructPose(CamPose* out);

// Moments that should be shown in third person (see cinematic_watch.cpp): while any of these "reasons" is
// active, first-person is switched off; when the last one clears it comes back if it was on before -- unless
// you toggled it yourself in the meantime. Exploration (walking, swimming) never sets a reason.
// GameCamera: the game is steering our camera (adjusting or replacing our writes; squeezing through gaps, scripted
// climbs such as the opening gorge) -- detected from the camera read-back, not from an event.
enum class ThirdPersonReason { Cinematic, WallCrawl, Grapple, Rope, Ledge, Interact, Rappel, Finisher, Diagnostic, GameCamera, Count };
void CameraReasonBegin(ThirdPersonReason reason, double timeoutSeconds);   // active until End, or the timeout
void CameraReasonPulse(ThirdPersonReason reason, double holdSeconds);      // active for a while (extends if repeated)
void CameraReasonEnd(ThirdPersonReason reason);
void FlushCameraTraceIfReady();

} // namespace vrmod
