#pragma once

#include "build_config.h"

namespace vrmod {

// Diagnostic: hooks the game's per-frame update for its "CameraBoneCameraController" (used, we believe, for
// cutscene/dialogue camera shots attached to a skeleton bone -- see project memory). Logs the joint index it's
// bound to and the resulting camera pose it writes, whenever one of these controllers ticks. The joint index for
// a head-attached shot is the piece of information we're after: once known, it may let us read Lara's real head
// transform every frame instead of the fixed-offset first-person camera.
#if VRMOD_DIAG
bool InstallCameraBoneProbe();
#else
inline bool InstallCameraBoneProbe() { return true; }
#endif

}
