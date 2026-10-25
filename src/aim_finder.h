#pragma once

#include "build_config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vrmod {

// A0 (docs/AIM.md): finds the game camera controller's yaw/pitch. Ctrl+Alt+Y, then turn the camera with the mouse and
// pause after each turn; writes vrmod_aimfind_<time>.txt. Present thread, once per frame.
#if VRMOD_DIAG
void AimFinderOnPresent();
#else
inline void AimFinderOnPresent() {}
#endif

// A0g: up to four of the last finder run's matches for the write watch (aim_watch.h): one per root first, then one
// per object, then any; angles before quaternions before forward vectors. Empty before a run.
struct AimWatchTarget { uintptr_t addr; std::string what; };
#if VRMOD_DIAG
std::vector<AimWatchTarget> AimFinderWatchTargets();
#else
inline std::vector<AimWatchTarget> AimFinderWatchTargets() { return {}; }
#endif

}
