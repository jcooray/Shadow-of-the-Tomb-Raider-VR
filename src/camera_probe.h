#pragma once

#include "build_config.h"

namespace vrmod {

// Reads a fixed set of candidate addresses (found via Cheat Engine pointer
// scanning, expressed as SOTTR.exe+offset so they survive game restarts) and
// logs their live float values. Tap F10 in-game to sample once; hold it to
// sample continuously while moving, so we can correlate values with motion.
#if VRMOD_DIAG
void ProbeStaticCandidatesIfHotkeyDown();
#else
inline void ProbeStaticCandidatesIfHotkeyDown() {}
#endif

} // namespace vrmod
