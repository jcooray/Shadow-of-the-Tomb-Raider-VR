#pragma once

#include "build_config.h"

namespace vrmod {

// Diagnostic: hooks cdc::BaseInstanceBoneAnchor's position accessor (the generic "resolve this instance+bone-
// index to a live world position" call the engine uses broadly -- markers, footstep sounds, the camera-vs-bone
// yaw system -- not just cutscenes). Logs which (instance handle, bone index) pairs are actually queried during
// normal play, and periodically summarises each handle's highest bone (by world Z) as a first guess at "head".
// The goal: identify Lara's own instance handle and her head bone's index empirically, since neither is a
// constant we can find in the executable (the mapping is per-skeleton content data, resolved only at runtime).
#if VRMOD_DIAG
bool InstallBoneAccessorProbe();
#else
inline bool InstallBoneAccessorProbe() { return true; }
#endif

}
