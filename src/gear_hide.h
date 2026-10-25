#pragma once

#include "build_config.h"

#include <cstdint>

namespace vrmod {

// Workstream G (docs/FIRSTPERSON.md, "Gear hiding"): hides the objects attached to Lara -- bow, quiver, axe, rifles
// on her back -- in first person, through the game's own switch. An attached instance's parent keeps a list of its
// attachments (the parent's component at index *(u32*)0x1416BB118: count at +0x20, 0x18-byte entries at +0x28, the
// child's +0x68 handle at +0 and a visible byte at +0x16); FUN_1406e2510, which the animation graph's
// AGInstanceHideUnhide node calls, just writes that byte. We write the same byte for Lara's attachments while first
// person is on (head camera locked) and she isn't aiming, and put the game's values back otherwise.
//
// [firstperson] hide_gear=1 (default). Ctrl+Alt+G cycles: all attachments hidden -> only attachment 1 hidden -> ... ->
// none hidden (off); every step logs the list (GEARHIDE lines) with each attachment's component classes.
#if VRMOD_DIAG
void GearHideGameThread(uint64_t gameFrame, bool firstPersonActive);   // camera hook, every write (once per frame)
#else
inline void GearHideGameThread(uint64_t gameFrame, bool firstPersonActive) {}
#endif
#if VRMOD_DIAG
void GearHideOnPresent();                                              // Present thread: hotkey, log lines
#else
inline void GearHideOnPresent() {}
#endif
// RETICLE watch (rope arrows): the game's reticle / rope target per frame while aiming -- changes and a 1 s summary.
#if VRMOD_DIAG
void ReticleWatchGameThread(uint64_t gameFrame, uintptr_t cameraComponent);   // camera hook, once per frame
#else
inline void ReticleWatchGameThread(uint64_t gameFrame, uintptr_t cameraComponent) {}
#endif
#if VRMOD_DIAG
void ReticleWatchOnPresent();
#else
inline void ReticleWatchOnPresent() {}
#endif

}
