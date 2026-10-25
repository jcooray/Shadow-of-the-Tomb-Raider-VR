#pragma once

#include <d3d12.h>
#include <cstdint>

namespace vrmod {

// Measures how long the GPU really spends on each game frame, independent of the frame rate the VR runtime locks
// the game to. Per frame: busy = (fence completion time) - max(previous completion, first submit of this frame).
// Start it once the game's queue and our copy fence exist; every real Present then reports its fence value.
bool GpuFrameTimerStart(ID3D12CommandQueue* gameQueue, ID3D12Fence* fence);
void GpuFrameTimerStop();

// Bracket our own ExecuteCommandLists call: its time is reported separately as our eye copy.
void GpuFrameTimerOurSubmit(bool begin);

// Called right after the Signal(fenceValue) that follows this frame's copy.
void GpuFrameTimerFrameSubmitted(uint64_t fenceValue, uint64_t presentFrame);

// Stage timestamps around the game's DLSS evaluate, on its command list (render thread): 0 before our motion-vector
// passes (D2), 1 before the evaluate (DLSS, and Cheeky/NR inside it), 2 after it. GPUSTAGES then splits each frame's GPU
// time into the game before DLSS, our MV passes, DLSS, the game after DLSS, and our eye copy.
void GpuFrameTimerDlssMark(ID3D12GraphicsCommandList* list, int point);

}
