#pragma once

#include <cstdint>

namespace vrmod {

// Workstream A, real fix: the head's yaw/pitch turn the game camera itself (the camera controllers' yaw/pitch state),
// in first person; see head_drive.cpp. [aim] head_drive, Ctrl+Alt+E toggles.
// Camera hook, after head aim's undo: `quat` (the game's rotation) becomes the body rotation the view is built on;
// returns true while driving. `eligible`: AER, first person with the head camera, no menu / cutscene / third-person
// moment.
bool HeadDriveGameThread(uintptr_t cameraComponent, uint64_t gameFrame, float quat[4], bool eligible);
bool HeadDriveActive();                 // head aim's record injection stands down while this is true
// Rope-shot hold: the head turn (camera-local, yaw then pitch) applied when the rope-shot camera took over; head aim
// injects only the change since then. False when not holding.
bool HeadDriveRopeHead(float q[4]);
// The view (real head included) points more than [firstperson] hide_body_deg from where Lara's body faces (10 deg
// hysteresis) -- her body isn't drawn then (camera_interceptor.cpp). False when head drive isn't driving.
bool HeadDriveLookingBack();
// The view centre written last game frame (body and real head), world yaw / pitch in radians (pitch up positive).
// False when head drive isn't driving. (Neck model, camera_interceptor.cpp.)
bool HeadDriveViewAngles(float* yaw, float* pitch);
void HeadDriveOnPresent();              // Present thread: hotkey, 30 s log line
void HeadDriveToggle(const char* how);
bool HeadDriveEnabled();

}
