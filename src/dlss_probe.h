#pragma once

#include <cstdint>

namespace vrmod {

// Workstream D0 (docs/DLSS.md): watches the game's own DLSS integration (NGX linked statically):
//   FUN_14101d140  ensure  -- creates the feature for a GPU node when needed (per frame)
//   FUN_14101b9c0  create  -- NGX CreateFeature for node n into the DLSS object's slot n (+0x28 + n*0x10)
//   FUN_14101b860  evaluate -- EvaluateFeature with the slot chosen by its last argument (the node)
// Logs each create (render size, output size, quality, flags), the ensure arguments when they change, the first
// evaluates in full (jitter, reset, render size, node, thread, latest game frame and its AER eye) and a 5 s summary.
// Hooks are byte-checked; nothing is changed.
bool InstallDlssProbe();
void DlssProbeOnPresent();   // Present thread: the summary line
// Per-eye DLSS history (in-headset overlay): -1 unavailable (no DLSS / second feature failed), 0 off, 1 on.
int DlssPerEyeState();
bool DlssPerEyeSet(bool on);

// The game's DLSS feature handles per eye while per-eye history (D1) is active -- slot 0 left, slot 1 right -- and the
// DLSS output size (for the foveated-DLSS host interface, cheeky_host.cpp). False otherwise.
bool DlssEyeHandles(uint64_t* left, uint64_t* right, uint32_t* outW, uint32_t* outH);

}
