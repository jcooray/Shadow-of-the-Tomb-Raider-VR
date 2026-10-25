#pragma once

#include "build_config.h"

namespace vrmod {

// Shadow of the Tomb Raider (Foundation engine, inherited from Rise of the
// Tomb Raider's 2016 stereoscopic 3D update) has a real, native dual-eye
// rendering mode -- not a driver-level automatic-stereo hack. It's normally
// only reachable via the in-game 3D display options UI, but that UI hides/
// disables the option in DX12 mode. A published tool
// (TimFx7/DX12-3DVision-Tomb-Raider-2.0, external OpenProcess/WriteProcessMemory)
// forces it on anyway by writing directly into the game's live options struct:
// moduleBase+0x36905B0 -> +0x90 -> +0x68 -> +0x68 -> +0x30 -> +0x38 -> +0xB0 -> +0x924,
// value 0=off, 1=side-by-side, 2=3D Vision (full per-eye stereo pairs).
//
// If the game's own native code renders the second eye, it issues its own
// correct OMSetRenderTargets calls per eye -- sidestepping the "RTV bindings
// are baked into a command list at record time" wall found while probing a
// DIY command-list-replay approach. Worth testing before any further custom
// render-hooking RE.
//
// That offset chain is ~4 years old (last known working against an earlier
// game build) and may not resolve correctly against the current Steam
// Definitive Edition build, the same way the camera struct's RVA had shifted
// from the reference project's documented offset. All reads/writes here are
// SEH-guarded so a stale/wrong chain fails safely (logged) instead of
// crashing the game.
#if VRMOD_DIAG
bool InstallStereoModeProbe();
#else
inline bool InstallStereoModeProbe() { return true; }
#endif

// Call once per frame from hkPresent, before calling the real Present.
// F6 (edge-triggered): read-only, logs each hop of the pointer chain and the
// final resolved value to vrmod.log -- use this first to check whether the
// chain still resolves sanely against this build. (Not F12 -- that's Steam's
// default screenshot hotkey.)
// F7 (edge-triggered toggle): forces the resolved value to 2 (3D Vision)
// every frame while enabled, restoring the value read at the time of the
// first successful resolve when toggled back off.
#if VRMOD_DIAG
void PollStereoModeHotkeys();
#else
inline void PollStereoModeHotkeys() {}
#endif

} // namespace vrmod
