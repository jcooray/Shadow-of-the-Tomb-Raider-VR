#pragma once

#include "build_config.h"

#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace vrmod {

// Phase 3 ghost test (Ctrl+G, in gameplay, head still). Measures whether anything in the game's frame reuses the
// previous frame -- which alternate-eye rendering would turn into a ghost of the other eye's view.
//
// The camera is driven through three stretches using the flip test's two poses (+60 / -60 degrees pitch):
//   hold +60 (settle, capture 3)   hold -60 (settle, capture 3)   alternate every frame (settle, capture 12)
//   hold +60 again (settle, capture 2; checks the scene did not change during the test)
// Each capture is the presented backbuffer, reduced to a ~512-pixel-wide thumbnail. Every alternating frame is then
// fitted as  A = P + w (N - P)  against the held references P (+60) and N (-60): w says which pose it is and how much
// of the other pose is mixed in (the ghost). The remaining difference from its own reference, relative to the
// frame-to-frame noise of the held frames, catches history reuse that is not a plain blend (TAA, motion blur).
//
// Writes vrmod_ghost_<time>.txt (numbers, verdict, the game's settings) and vrmod_ghost_<time>.bmp (contact sheet)
// next to the exe, and a summary to vrmod.log. Call once per Present on the game's queue, after the frame is done.
#if VRMOD_DIAG
void GhostTestOnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer);
#else
inline void GhostTestOnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) {}
#endif

// True while the test runs and for a few frames after: the headset images are cleared to black (the view flips
// +-60 degrees up to 72 times a second, which must not be shown), and head tracking is frozen for the whole run so
// the headset can stay on (it may power down if taken off).
#if VRMOD_DIAG
bool GhostTestBlackout();
#else
inline bool GhostTestBlackout() { return false; }
#endif

// Logs which of the game's settings reuse the previous frame (from the settings block in its own log).
// Returns the text it logged.
#if VRMOD_DIAG
std::string TemporalSettingsReport();
#else
inline std::string TemporalSettingsReport() { return {}; }
#endif

} // namespace vrmod
