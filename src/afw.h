#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace vrmod {

// AFW (docs/AFW.md): the eye not rendered this frame is rebuilt from the one that was (depth-based stereo warp), so
// both eyes update every frame under AER. [render] afw, Ctrl+Alt+I toggles.
bool AfwEnabled();
void AfwSetEnabled(bool on);   // in-headset overlay
// The angle between the headset's two eye views (each frame): over 0.5 deg the warp stands down (logged on a change).
void AfwSetEyeViewAngle(float degrees);
bool AfwCantedEyes();   // standing down for that reason
// DLSS evaluate (render thread), per-eye routing known: linearises the frame's depth into a slot kept for its Present.
void AfwOnEvaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, uint32_t renderW, uint32_t renderH, uint64_t frame);
// Present (our copy list): backbuffer in COPY_SOURCE, dst (the other eye's swapchain image) in COPY_DEST. baselineCm:
// the other eye's offset along the camera's x axis (game cm, positive = to the right); tanH/tanV: the frame's frustum. False if no depth for `frame` (caller falls back).
bool AfwRecordWarp(ID3D12GraphicsCommandList* cl, ID3D12Resource* backbuffer, ID3D12Resource* dst, uint64_t frame, float baselineCm,
                   float tanH, float tanV, int renderedEye);
void AfwOnPresent();   // hotkey, 10 s log line
bool AfwReady(uint64_t frame);   // enabled and the frame's depth was captured (decide before acquiring the other eye)

}
