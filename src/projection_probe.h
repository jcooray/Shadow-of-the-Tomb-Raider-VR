#pragma once

#include "build_config.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace vrmod {

// Phase 6 -- what frustum does the game actually render? (Ctrl+O, AER mode, in gameplay, ~3 s.)
//
// Holds the game camera, freezes the head pose and renders the left eye only, turned by +-2 degrees yaw and then
// +-2 degrees pitch (camera-local). For each pair it reads back a 2048 x 32 horizontal strip / 32 x 2048 vertical
// strip through the image centre and finds the image shift s by profile matching. At the centre s = 2 f tan(2 deg),
// so f (pixels per unit tangent) and the half-angle tangents follow: tanH = (W/2)/f_x, tanV = (H/2)/f_y. These are
// compared with every FOV model's prediction for the FOV value written into the game, and with the frustum being
// declared to the runtime. Writes vrmod_projection_<time>.txt. Call once per Present (Present thread).
#if VRMOD_DIAG
void ProjectionProbeOnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer);
#else
inline void ProjectionProbeOnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) {}
#endif

} // namespace vrmod
