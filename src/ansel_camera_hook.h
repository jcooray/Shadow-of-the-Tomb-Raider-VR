#pragma once

#include "build_config.h"

namespace vrmod {

// Hooks the game's own wrapper around ansel::updateCamera(Camera&) -- found via
// Ghidra decompilation at RVA 0x100f850 (verified, not guessed): the call site
// is literally `ansel::updateCamera((Camera *)(param_1 + 0xc))`. NVIDIA's
// Ansel SDK Camera struct has a publicly documented layout (position, then a
// quaternion rotation, then FOV/projection offsets), so this should give us
// exactly the position+rotation data we've been hunting for all session,
// without needing to touch the game's own internal (obfuscated) camera class.
#if VRMOD_DIAG
bool InstallAnselCameraHook();
#else
inline bool InstallAnselCameraHook() { return true; }
#endif

} // namespace vrmod
