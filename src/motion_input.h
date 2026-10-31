#pragma once

#include <windows.h>
#include <xinput.h>

#include <cstdint>

namespace vrmod {

// Motion controllers as a gamepad (docs/MOTION_CONTROLS.md, stage 1). The OpenXR side owns every action set of the
// session: eye gaze's and the controllers' are attached together (a session attaches action sets only once) and
// synced together once per frame. The controllers' sticks, triggers, grips and buttons become an XInput gamepad state
// that overlay_pad.cpp merges into what the game reads (XInput and Windows.Gaming.Input), alongside any real gamepad.
//
// Index controllers: left A / B = X / Y, a swipe on the left trackpad = D-pad, pressing it = View, right trackpad
// press = Menu (held 1.5 s: View), both trackpads pressed together = the overlay. Touch controllers: left Menu = Menu (held
// 1.5 s: View), right stick click held + left stick = D-pad, Menu + right stick click = the overlay.

// After xrCreateSession: creates eye gaze's actions (when eyeGaze) and the controllers', and attaches them.
void MotionInputInitSession(void* instance, void* session, uint64_t systemId, bool eyeGaze);
// Once per frame, after xrWaitFrame: syncs the actions, updates eye gaze and the virtual gamepad, and locates the
// controllers' aim poses in appSpace (the XrSpace the views are located in).
void MotionInputFrame(int64_t predictedDisplayTime, void* appSpace);
// A controller's aim pose (hand 0 left, 1 right) in that space -- OpenXR axes, -Z along the pointing ray; false when
// it isn't tracked (or not in the last 250 ms).
bool MotionHandAim(int hand, float q[4], float p[3]);
void MotionInputShutdown();

// The virtual gamepad for the game (no Guide, nothing of the overlay chord): false when the controllers aren't in use
// (off in the settings, not focused, the game not in front, or no fresh state).
bool MotionPadGame(XINPUT_GAMEPAD* out);
// The same for the overlay, with Guide + Menu while the overlay chord is held.
bool MotionPadOverlay(XINPUT_GAMEPAD* out);
// True once the controllers have been in use this session: XInput slot 0 then reports a connected pad.
bool MotionPadEverUsed();
// overlay_pad.cpp counts the game's reads that got the virtual pad (0 XInput, 1 Windows.Gaming.Input).
void MotionPadNoteDelivered(int api);

} // namespace vrmod
