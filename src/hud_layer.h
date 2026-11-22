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
bool HudLayerRedirect(const void* resource, D3D12_CPU_DESCRIPTOR_HANDLE* ourRtv, bool clear = false);   // bind / clear of the game's layer -> ours?
// World icons (item / collectible markers): Scaleform drawn into the full-resolution HDR scene, depth-tested -- warped by
// AFW with the depth behind them. ui_probe draws them into our layer instead (cloned pipelines); true + our RTV if active.
bool HudLayerWorldRtv(D3D12_CPU_DESCRIPTOR_HANDLE* ourRtv);
// Present path (openxr_integration.cpp), after both eye images are written, eyes in RENDER_TARGET state. Null eyes skipped.
// renderedEye: the eye this game frame was rendered for (AER: the game camera sits at that eye, and the game places
// world-anchored icons with it), -1 = both (mono). Each eye gets the HUD of the last frame rendered for it.
// eyeW x eyeH: the eye image's sub-rectangle (top-left) of the swapchain image, which can be larger (its capacity).
// `panel`: this frame's HUD panel targets (below), or null = the HUD goes over the eye images (as until 1.1.1).
struct HudPanelTargets;
void HudLayerComposite(ID3D12GraphicsCommandList* cl, ID3D12Resource* const eyes[2], int renderedEye, UINT eyeW, UINT eyeH,
                       HudPanelTargets* panel = nullptr);
bool HudLayerEnabled();

// ---- HUD panel (docs/HUD.md, "HUD panel", 2026-11-15) ----
// The screen HUD as a layer of its own in the headset (an OpenXR quad, head-locked, [hud] distance_m away) instead of
// pixels in each eye's image: the runtime places it, so it sits in the same place on every headset whatever its eye
// images' size, field of view or canting, and it is sampled at the headset's own resolution. While aiming, the reticle
// -- a box at the HUD's centre -- is cut out of the panel and shown at the aim's hit point, at its real distance, in a
// second quad: the rest of the HUD stays where it is (until now the whole HUD was shifted to put its centre on the hit).
// World icons (drawn into the scene with its depth) stay in the eye images, from a texture of their own.
//
// Present thread, each frame: HudLayerPanelPlan says what the composite will draw (so openxr_integration acquires those
// swapchain images), HudLayerComposite draws them and reports back.
struct HudPanelTargets {
    ID3D12Resource* panel = nullptr;     // the acquired panel image (RENDER_TARGET state), or null: not updated this frame
    ID3D12Resource* reticle = nullptr;   // the acquired reticle image, or null
    bool panelDrawn = false, reticleDrawn = false;   // out
};
struct HudPanelPlan {
    bool active = false;       // the panel route is in use this frame (pass targets to the composite)
    bool panel = false;        // the panel image is to be updated (acquire it)
    bool reticle = false;      // ...and the reticle image
    bool empty = false;        // nothing to show from now on (no HUD drawn, a menu): hide both quads
};
// renderedEye as for the composite. allow: the caller can show the panel this frame (swapchains exist, no screen mode);
// allowReticle: it can place the reticle (alternate-eye frames, whose poses it knows).
HudPanelPlan HudLayerPanelPlan(int renderedEye, bool allow, bool allowReticle);
bool HudLayerPanelWanted();                    // [hud] panel on, the layer on, and the panel not failed
void HudLayerPanelFailed(const char* why);     // the runtime can't show it: the HUD goes back into the eye images
// Sizes for the two swapchains (pixels); false until the game's HUD layer has been seen.
bool HudLayerPanelPixels(uint32_t* panelW, uint32_t* panelH, uint32_t* reticle);
// The panel's place in the game's frustum: its half-size as a share of the frustum's (the HUD scale), its centre's
// offset down as a share of the frustum's height, and the reticle box's half-size as a share of the frustum's height.
void HudLayerPanelShape(float* scale, float* offsetDown, float* reticleHalf);
// ui_probe, the game clearing its HUD layer: the world-icon texture is cleared with it (true + its RTV).
bool HudLayerWorldClear(const void* resource, D3D12_CPU_DESCRIPTOR_HANDLE* worldRtv);
// The reticle. Camera hook (game thread), once per game frame: on = aiming; local = the aim's hit point from that frame's
// camera in its own axes (X right, Y forward, Z up; game units), or only a direction (hasDistance false); fromCentre =
// along the view's centre from between the eyes (head aim: the game aims along whichever eye's camera the frame has).
void HudLayerSetReticle(uint64_t gameFrame, bool on, const float local[3], bool hasDistance, bool fromCentre);
struct HudReticle { bool on; float local[3]; bool hasDistance, fromCentre; };
bool HudLayerReticleFor(uint64_t gameFrame, HudReticle* out);   // present thread; false if that frame isn't known
void HudLayerSetEnabled(bool on);   // in-headset overlay
void HudLayerOnPresent();   // Ctrl+Alt+L cycle (on / on red-tinted test / off), 30 s log line

}  // namespace vrmod
