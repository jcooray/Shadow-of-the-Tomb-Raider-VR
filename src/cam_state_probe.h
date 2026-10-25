#pragma once

#include "build_config.h"

#include <cstdint>

namespace vrmod {

// Workstream A, real fix: Ctrl+Alt+C samples the camera component and its controllers for ~12 s and logs the floats
// that track the game camera's own yaw/pitch (CAMSTATE lines) -- candidates for the state the right stick integrates.
#if VRMOD_DIAG
void CamStateProbeGameThread(uintptr_t cameraComponent, const float gameQuatWithoutHead[4]);   // camera hook, per write
#else
inline void CamStateProbeGameThread(uintptr_t cameraComponent, const float gameQuatWithoutHead[4]) {}
#endif
#if VRMOD_DIAG
void CamStateProbeOnPresent();                                                                   // Present thread
#else
inline void CamStateProbeOnPresent() {}
#endif

}
