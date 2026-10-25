#pragma once

#include <d3d12.h>
#include <cstdint>
#include <string>

namespace vrmod {

// Lara's head position as the GPU draws it (docs/FIRSTPERSON.md, "Head lock from the face draw"): read from the face
// draw's own constants -- skinning matrix 0 (CB_Skinning, b1) applied to the face's model-space centre, checked against
// the model's World matrix (WorldBuffer, b0). Lets the skeleton search start at her real head instead of the offset
// camera, whatever her pose.

// Called by ui_probe's HookDevice: descriptor-heap / CBV / copy / root-table / Map hooks.
void FaceTrackInstall(ID3D12Device* dev, void** deviceVtable, void** listVtable);
// Called by ui_probe at the head's face draw (71256 indices from 0) in a colour pass, before it is skipped.
void FaceTrackOnFaceDraw(const void* list);
// Called by ui_probe when a command list is reset.
void FaceTrackForgetList(const void* list);
// The latest head point (world units, cm) if it is younger than maxAgeMs.
bool FaceHeadPoint(float out[3], double maxAgeMs);
// The model root from the same draw (its World matrix), if younger than maxAgeMs: position (cm) and the world yaw of its
// local X and Y axes (atan2(-x, y), as the camera's yaw) -- which one is her forward is learned by the look limit.
bool FaceRootPose(float pos[3], float axisYaw[2], double maxAgeMs);
// True if [addr, addr+size) lies in a GPU buffer the game has mapped (an upload heap: draw data, refilled every frame).
bool FaceTrackIsMappedGpuMemory(uint64_t addr, uint64_t size);
// The constants a draw in this list will use: its root descriptor table (param 2) and the WorldBuffer (b0) and
// CB_Skinning (b1) CBV addresses in it. False if unknown. (Body census, ui_probe.cpp.)
bool FaceTrackDrawKeys(const void* list, uint64_t* table, uint64_t* b0, uint64_t* b1);
// Body learning (2026-10-12): Lara's body / outfit index buffers, learned every 3 s as the draws whose World matrix
// equals her face's (any outfit). ui_probe feeds the learning frame's colour draws; the Present thread ticks it.
bool FaceTrackBodyLearning();
void FaceTrackLearnDraw(const void* list, uint64_t ib, uint32_t count, uint32_t first);
void FaceTrackBodyTick(uint64_t faceIb);
bool FaceTrackIsBodyIb(uint64_t ib);
// One-line statistics since the last call (log).
std::string FaceTrackStats();

}  // namespace vrmod
