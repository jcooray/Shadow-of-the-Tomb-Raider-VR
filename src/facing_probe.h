#pragma once

#include "build_config.h"

namespace vrmod {

// Where does the game keep Lara's facing? (Ctrl+Alt+F12, standing still in first-person gameplay; 2026-10-11, for
// turning her body with the view.) Her model's root pose comes from the face draw's World matrix (face_track.cpp).
// Scans writable memory for her exact position, keeps the hits with a rotation beside them that matches her model's
// facing (matrix rows / columns, a quaternion) plus nearby floats equal to her yaw, then tests each one: turns it by
// 15 deg, watches over 6 Presents whether her drawn model turned and whether the value stayed, and puts it back.
// Results in the log (FACINGPROBE) and vrmod_facingprobe_<time>.txt. Call once per Present.
#if VRMOD_DIAG
void FacingProbeOnPresent();
// The game thread (the camera write hook): carries out the probe's pending write / set-rotation call.
void FacingProbeGameThread();
#else
inline void FacingProbeOnPresent() {}
inline void FacingProbeGameThread() {}
#endif

}  // namespace vrmod
