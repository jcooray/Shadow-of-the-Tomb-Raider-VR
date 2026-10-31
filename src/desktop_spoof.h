#pragma once

#include <windows.h>

struct IDXGIOutput;

namespace vrmod {

// Diagnostic/experimental: when a file named vrmod_fakedesktop.txt exists next to
// the exe, the game is told the primary monitor is bigger than it really is
// (default 7680x4320; the file may contain "<width> <height>"). In fullscreen
// window the game sizes its window and swapchain to the desktop, so this makes
// it create a genuinely large swapchain (a borderless window bigger than the
// monitor) instead of downscaling its internal render target to the desktop.
// Does nothing, and installs no hooks, if the file is absent.
bool InstallDesktopSpoof();

// Hooks IDXGIOutput::GetDesc on the given output's vtable (same vtable is shared
// by all outputs). Call after InstallDesktopSpoof().
void DesktopSpoofHookOutput(IDXGIOutput* output);

// The real (unspoofed) current desktop mode, for code that must not see the fake.
void RealDesktopMode(UINT* width, UINT* height);

// True when the fake desktop is not 16:9 and GetDeviceCaps is also spoofed, so the game's "monitor aspect ratio"
// (it is HORZSIZE/VERTSIZE in millimetres) follows the fake desktop's aspect. Valid after InstallDesktopSpoof().
bool DesktopSpoofAspectOverridden();
// The fake desktop's size (the swapchain the game is asked for); false when the spoof isn't active.
bool DesktopSpoofSize(unsigned* w, unsigned* h);

// The render size for a percentage of the headset's target (pixels, like SteamVR's), in multiples of 8 and at most
// 8192 on either side (DLSS's limit). True when it had to be scaled down to fit.
bool RenderSizeForPct(float pct, float targetW, float targetH, unsigned* w, unsigned* h);

// The game created its window swap chain (any size, called from SOTTR.exe). This start's render resolution is kept once
// the game's DLSS is created (DesktopSpoofDlssCreated) or 20 s later; a start that closes before then makes the next
// start step the render resolution down (desktop_spoof.cpp, self-healing).
void DesktopSpoofGameWindowCreated(unsigned w, unsigned h);
// The game's DLSS create returned (its output size).
void DesktopSpoofDlssCreated(bool ok, unsigned outW, unsigned outH);
// This start stepped the render resolution down because the last one never reached the game's window: from that
// percentage to 100 %, or to 0 = the game's own resolution. False when it didn't.
bool DesktopSpoofFallback(int* fromPct, int* toPct);

} // namespace vrmod
