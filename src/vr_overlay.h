#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrmod {

// In-headset overlay (docs/OVERLAY.md): Menu + Guide on the gamepad opens a panel in front of you with the mod's status
// and settings, on six pages (Status, View, Rendering, HUD, Controls, Support). The game keeps running; its gamepad
// input is held while the panel is open (overlay_pad.cpp). Changes apply at once where the mod can and are saved to
// vrmod_settings.ini; the others are marked "after a restart".
//
// The panel is drawn on the CPU (GDI, into a BGRA bitmap) only when its content changes; the OpenXR side uploads that
// image to a swapchain of its own and submits it as a quad layer over everything else (openxr_integration.cpp).

// What the OpenXR side knows, for the Status and Rendering pages (set every frame it runs).
struct OverlayXrInfo {
    std::string runtime, system;
    float refreshHz = 0.0f;
    std::vector<float> rates;   // supported refresh rates (XR_FB_display_refresh_rate), ascending; empty = no control
    uint32_t eyeW = 0, eyeH = 0;
    bool screenMode = false;
};
void VrOverlaySetXrInfo(const OverlayXrInfo& info);

void VrOverlayTick();                 // Present thread, once per frame: gamepad, pages, drawing when changed
bool VrOverlayVisible();
bool VrOverlayTakePlace();            // true once per opening (and after a distance change): place it in front of the head
const uint8_t* VrOverlayTakeImage(int* w, int* h);   // the panel (BGRA, top-down, straight alpha) when it changed, else nullptr
float VrOverlayWidthM();
float VrOverlayDistanceM();

// A message in front of you for a few seconds, with the panel closed (with it open: in its help line).
void VrOverlayNotify(const std::string& text, int seconds = 8);

// Requests to the OpenXR side, each taken once.
float VrOverlayTakeRefreshRequest();  // a refresh rate to request (Hz), 0 = none
bool VrOverlayTakeScreenModeToggle(); // flat screen mode on/off

}  // namespace vrmod
