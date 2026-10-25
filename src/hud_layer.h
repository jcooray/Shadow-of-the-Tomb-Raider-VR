#pragma once

#include <d3d12.h>
#include <cstdint>

namespace vrmod {

// HUD layer (docs/HUD.md, "HUD layer"): the game draws its whole UI into its own RGBA8 sRGB layer and blends it over
// the tone-mapped scene in its final composite (scene * (1 - a) + ui, linear). Under AFW the other eye is warped from
// the finished image, so the HUD moved with the scene depth under it -- world-anchored icons flickered and ghosted.
// Here the game's UI draws are redirected into a texture of ours (the game's layer stays empty, so its composite and
// the backbuffer hold the scene only) and that texture is blended over each eye image after the warp.

// ui_probe hooks (render threads):
void HudLayerSeeUiDraw(ID3D12Resource* gameLayer);                                  // a UI-pipeline draw into an sRGB target
bool HudLayerRedirect(const void* resource, D3D12_CPU_DESCRIPTOR_HANDLE* ourRtv);   // bind / clear of the game's layer -> ours?
// World icons (item / collectible markers): Scaleform drawn into the full-resolution HDR scene, depth-tested -- warped by
// AFW with the depth behind them. ui_probe draws them into our layer instead (cloned pipelines); true + our RTV if active.
bool HudLayerWorldRtv(D3D12_CPU_DESCRIPTOR_HANDLE* ourRtv);
// Present path (openxr_integration.cpp), after both eye images are written, eyes in RENDER_TARGET state. Null eyes skipped.
// renderedEye: the eye this game frame was rendered for (AER: the game camera sits at that eye, and the game places
// world-anchored icons with it), -1 = both (mono). Each eye gets the HUD of the last frame rendered for it.
void HudLayerComposite(ID3D12GraphicsCommandList* cl, ID3D12Resource* const eyes[2], int renderedEye);
bool HudLayerEnabled();
void HudLayerSetEnabled(bool on);   // in-headset overlay
void HudLayerOnPresent();   // Ctrl+Alt+L cycle (on / on red-tinted test / off), 30 s log line

}  // namespace vrmod
