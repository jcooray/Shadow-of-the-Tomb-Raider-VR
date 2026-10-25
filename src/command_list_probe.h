#pragma once

#include "build_config.h"

namespace vrmod {

// Hooks the D3D12 command-list submission pipeline (ExecuteCommandLists,
// command-list Reset/Close/OMSetRenderTargets/Draw*) purely for structural
// diagnostics: which render targets does each command list draw into, how
// many draw calls per target, and how many command lists get batched into
// each ExecuteCommandLists call. This is the de-risking step for a stereo
// "replay the same command list against a second eye's camera data" approach
// -- we need to know what a frame's command-list structure actually looks
// like (which pass is the main 3D scene vs. UI/post-process) before deciding
// whether replay or some other technique is viable.
#if VRMOD_DIAG
bool InstallCommandListProbe();
#else
inline bool InstallCommandListProbe() { return true; }
#endif

// Call once per frame from hkPresent, before calling the real Present. Polls
// the F10 hotkey (edge-triggered) to arm a one-shot capture of the next
// several frames' command-list structure, and flushes+logs the capture to
// vrmod.log once the span completes.
#if VRMOD_DIAG
void PollCommandListCaptureHotkey();
#else
inline void PollCommandListCaptureHotkey() {}
#endif

} // namespace vrmod
