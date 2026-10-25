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
};

SteamVrScale ReadSteamVrScale();

}
