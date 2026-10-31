#pragma once
// The graphics card the headset renders on, and a starting render resolution for it.
#include <windows.h>
#include <cstdint>
#include <string>

namespace vrmod {

struct GpuInfo {
    std::string name;          // DXGI's adapter description, e.g. "NVIDIA GeForce RTX 4070"
    uint64_t vramMB = 0;
    float relative = 0.0f;     // rough speed at high resolutions against the development RTX 5090 (1.0)
    bool known = false;        // relative is from the table, not the fallback guess
};

// The adapter with this LUID (OpenXR's graphics requirements name it), through the system DXGI -- no device is created.
// Also remembered for DetectedGpuName.
GpuInfo GpuInfoForLuid(LUID luid);

// The name of the graphics card the headset renders on, once GpuInfoForLuid has run ("" before).
std::string DetectedGpuName();

// At the start of every log: the graphics cards (name, video memory, driver), the Windows build and the headset of the
// last VR session ([render] headset, written by the OpenXR side) -- so a log from a start that fails before VR begins
// still says what it ran on (1.0.1). Through the system DXGI, no device is created.
void LogSystemInfo();

// Foveated rendering (CheekyFoveatedDLSS) foveates only the DLSS pass -- the game's own rendering is unchanged -- so it
// changes the GPU time per frame by a part of DLSS's cost: very little fixed (its large sharp region needs two DLSS
// passes), much more eye tracked (a small one); DLSS 5 (neural rendering) on top takes most of that back.
enum class Foveation { Off, Fixed, Eye, EyeDlss5 };

// What the settings ask for and the game folder has: [foveation] mode and dlss5, CheekyFoveatedDLSS installed, DLSS 5's
// runtime (nvngx_dlssnr.dll) beside the game.
Foveation CurrentFoveation();
const char* FoveationName(Foveation f);
// Cheeky's sharp region (Width / Height, fractions of the image) for a mode: large when fixed, so a glance away from the
// middle stays sharp; small when it follows the eyes.
void FoveaSize(Foveation f, float* width, float* height);

// A render resolution (percent of the headset target's pixels, multiples of 10, 50..150) that should hold 72 Hz on this
// graphics card with the game at its usual settings.
int StartingResolutionPct(float relative, uint32_t targetW, uint32_t targetH, Foveation fov = Foveation::Off);

// The GPU time per frame (ms) the same model expects on this card at pct of the headset target.
double PredictedFrameMs(float relative, uint32_t targetW, uint32_t targetH, int pct, Foveation fov = Foveation::Off);

}  // namespace vrmod
