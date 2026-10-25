#pragma once

#include "build_config.h"

namespace vrmod {

// Phase 4 follow-up -- who reads the camera struct? (Ctrl+R, in gameplay, ~3 s; see docs/PHASE4.md.)
//
// Arms hardware data watchpoints (debug registers DR0-DR2, read/write, 4 bytes) on the camera struct's position
// (+0x80), rotation (+0xA0) and FOV (+0xB0) on every thread of the process, for ~3 seconds, and records every
// access in a vectored exception handler: the instruction, the thread, and the time. Afterwards the watchpoints are
// cleared and the results written:
//   vrmod_readers_<time>.txt : each accessing instruction (module+offset), how often, from which threads (named where
//                              Windows knows the name; the camera-write and Present threads are labelled), the code
//                              bytes around it, and when it runs relative to the Present;
//   vrmod_readers_<time>.csv : every access (time, instruction, thread, field) plus the Present times.
// The frame recorder runs during the probe, so its camwrites CSV gives the write times to line up with.
// Call once per Present (from the Present thread).
#if VRMOD_DIAG
void ReaderProbeOnPresent();
#else
inline void ReaderProbeOnPresent() {}
#endif

} // namespace vrmod
