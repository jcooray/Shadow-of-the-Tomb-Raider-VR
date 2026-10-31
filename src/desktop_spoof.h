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

// The game created its window swap chain (any size, called from SOTTR.exe): this start's render resolution works, so the
// next start keeps it. Without this, the next start steps the render resolution down (desktop_spoof.cpp, self-healing).
void DesktopSpoofGameWindowCreated(unsigned w, unsigned h);
// This start stepped the render resolution down because the last one never reached the game's window: from that
// percentage to 100 %, or to 0 = the game's own resolution. False when it didn't.
bool DesktopSpoofFallback(int* fromPct, int* toPct);

} // namespace vrmod
