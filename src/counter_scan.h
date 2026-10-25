#pragma once

#include "build_config.h"

namespace vrmod {

// Phase 4 -- find the engine's own frame counters in SOTTR.exe's .data section (Ctrl+N, in gameplay, ~6 s).
//
// Snapshots .data on 4 consecutive Presents and keeps every 32-bit value that went up by exactly 1 each time. Those
// are then read at each of the next 300 Presents together with the latest camera write's game frame G, while the
// game thread is held for 20 ms every 25 frames (CameraInjectStall) -- the "game thread late" case in which the
// presented frame is 1 instead of 2 game frames behind G. A counter that follows the game thread keeps a constant
// offset to G; one that follows the frame being rendered/presented shifts its offset after the stalls. Writes
// vrmod_counters_<time>.txt. Call once per Present (from the Present thread).
#if VRMOD_DIAG
void CounterScanOnPresent();
#else
inline void CounterScanOnPresent() {}
#endif

} // namespace vrmod
