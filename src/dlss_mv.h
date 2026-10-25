#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace vrmod {

// D2a (docs/DLSS.md): proves the motion-vector model before any correction is built. Ctrl+Alt+Q arms one capture:
// at the next DLSS evaluate a small compute pass (recorded into the game's command list, before DLSS) samples the
// game's motion vectors and depth on a 32x18 grid into our own buffer and copies it to a readback buffer. A few
// Presents later the samples are compared with what our recorded cameras predict (the frame's own camera and the
// previous frame's, which under AER is the other eye) for each depth convention (reverse-Z infinite / finite,
// forward-Z), sign and unit, and the ranking is logged -- with the size of the correction D2 would apply (same eye
// two frames back vs other eye one frame back).
void MvProbeOnEvaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, uint32_t renderW,
                       uint32_t renderH, uint64_t frame, int eye);
bool MvProbeArmed();
void MvProbeOnPresent();   // hotkeys, resolve and stats (Present thread)

// D2b: with per-eye history, rewrites the motion vectors DLSS gets so they point at the same eye two frames back
// (frame - 2) instead of the other eye one frame back; returns our motion-vector texture to pass to DLSS instead of
// the game's, or nullptr (not possible for this frame, or off -- Ctrl+Alt+F). Render thread, before the evaluate.
bool MvFixEnabled();

// D2c: the evaluated frame = AER's last paired frame + 1 + this offset (found from the game's own motion vectors; 0
// normally, other values after the render thread got ahead, e.g. after the pause menu). MvTakeOffsetChange: true once
// after a change (both histories must reset). MvFrameCheckOnEvaluate: samples for the check (render thread; every
// second call), rawFrame = the prediction without offset.
int MvFrameOffset();
bool MvTakeOffsetChange();
void MvFrameCheckOnEvaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, uint32_t renderW,
                            uint32_t renderH, uint64_t rawFrame);
ID3D12Resource* MvFixOnEvaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, uint32_t renderW,
                                uint32_t renderH, uint64_t frame);

}
