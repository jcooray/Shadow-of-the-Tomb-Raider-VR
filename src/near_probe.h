#pragma once

#include "build_config.h"

namespace vrmod {

// First person, hiding the head (docs/FIRSTPERSON.md, step 5): find the renderer's near clip distance.
// Ctrl+Alt+Z dumps the camera manager's memory around our camera struct (vrmod_camslot_<time>.txt) and lists every
// float equal to 5.0 (the culling plane says near = 5 cm); Ctrl+Alt+X writes 30 into those candidates one at a time.
#if VRMOD_DIAG
bool InstallNearProbe();
#else
inline bool InstallNearProbe() { return true; }
#endif
#if VRMOD_DIAG
void NearProbeOnPresent();   // Present thread, once per frame
#else
inline void NearProbeOnPresent() {}
#endif
#if VRMOD_DIAG
void NearProbeApply();       // also from the camera hook after each game write (the value may be rewritten per frame)
#else
inline void NearProbeApply() {}
#endif

}
