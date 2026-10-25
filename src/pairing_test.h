#pragma once

#include "build_config.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace vrmod {

// Phase 4, Test A -- camera-to-frame pairing (Ctrl+P, in gameplay; ~3 minutes; see docs/PHASE4.md).
//
// Measures N: how many game frames lie between our write of a camera pose and the Present of the frame that shows it.
// The camera is flipped +60 / -60 degrees pitch by a pseudo-random bit of each game frame (strict alternation would
// only give N modulo 2). Every Present, 16 small luminance patches of the backbuffer are copied back without
// stalling; the pose each frame shows is read from them against held +60 / -60 references, and matched with the
// written sequence at every candidate N.
//
// Four segments, each with its own held references, vary the pipeline (brief: "varying GPU load"):
//   default (framesAhead 1) / no CPU limit (framesAhead 0) / deep queue (framesAhead 3) / half rate.
// Headset black, head tracking frozen, third-person camera, and the per-frame recorder (Ctrl+J) running for the
// whole test (its CSVs carry the write-persistence data for Test B). Writes vrmod_pairing_<time>.txt and .csv.
#if VRMOD_DIAG
void PairingTestOnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer);
#else
inline void PairingTestOnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) {}
#endif

// True while the test runs and for a few frames after (the headset images are cleared to black).
#if VRMOD_DIAG
bool PairingTestBlackout();
#else
inline bool PairingTestBlackout() { return false; }
#endif

} // namespace vrmod
