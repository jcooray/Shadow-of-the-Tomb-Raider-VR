#pragma once

#include "build_config.h"

namespace vrmod {

// Phase 4 -- where do copies of the camera pose live, and how old are they at Present? (Ctrl+T, in gameplay.)
//
// Turns on the FOV pose tag (CameraSetFovTagging: the game frame's low 6 bits hidden in the FOV's low mantissa bits),
// waits for it to propagate, then scans all readable committed memory once for:
//   raw   : the FOV itself (bits equal apart from the tag)                     -> tag read directly
//   cot   : 1/tan(fov/2), the projection matrix's [1][1] (within a few ulp)    -> tag = nearest of the 64 candidates
//   tan   : tan(fov/2)                                                          -> likewise
// and then reads every hit at each of the next 120 Presents, giving per address how old its tag is relative to the
// latest camera write (the lag, in game frames) and how often it changes. The game freezes for a few seconds during
// the scan. Writes vrmod_valuescan_<time>.txt. Call once per Present (from the Present thread).
#if VRMOD_DIAG
void ValueScanOnPresent();
#else
inline void ValueScanOnPresent() {}
#endif

} // namespace vrmod
