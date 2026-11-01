#pragma once
// SteamVR's own render resolution setting, from Steam\config\steamvr.vrsettings: the global setting -- manual
// (steamvr.supersampleScale, 1.0 when not saved) or automatic for the GPU (GpuSpeed.gpuSpeedRenderTargetScale) -- times
// this game's per-app setting (steam.app.<id>.resolutionScale, %). SteamVR applies it to the size it recommends to the
// game; the mod takes it back out (openxr_integration.cpp UpdateProjectionMatch), so it doesn't change this game.

namespace vrmod {

struct SteamVrScale {
    bool read = false;       // the file was found and read (else SteamVR's settings are unknown: assume 100 %)
    bool manual = false;     // the global setting is manual (else SteamVR's automatic choice for the GPU)
    double global = 1.0;     // the global setting, a factor of pixels
    double perApp = 100.0;   // this game's per-app setting, % of pixels
    double Total() const { return global * perApp / 100.0; }   // factor of pixels SteamVR applies to this game
    // Motion Smoothing (2026-11-01): the global switch (steamvr.motionSmoothing; SteamVR's default is on) and this game's
    // override (motionSmoothingOverride: 0 use the global, 1 force on, 2 force off, 3 force always on -- the values of
    // SteamVR's own dashboard). Frame warp and Motion Smoothing both make in-between images, and fight each other.
    bool smoothingGlobal = true;
    int smoothingOverride = 0;
    bool SmoothingOn() const { return smoothingOverride == 1 || smoothingOverride == 3 || (smoothingOverride == 0 && smoothingGlobal); }
};

SteamVrScale ReadSteamVrScale();

}
