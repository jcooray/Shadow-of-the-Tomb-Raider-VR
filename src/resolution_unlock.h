#pragma once

#include <windows.h>

namespace vrmod {

// The game only accepts resolutions its DXGI display-mode enumeration
// returns (a registry value of 5120x2880 was ignored), and this machine has no
// DSR to add virtual ones. The game imports DXGI factories directly (not
// EnumDisplaySettings), so its list comes from IDXGIOutput::GetDisplayModeList
// / GetDisplayModeList1 -- hook those (and FindClosestMatchingMode, which a
// game may use to validate/snap a chosen mode) and append higher-resolution
// modes cloned from the largest real ones. Only the backbuffer matters to the
// VR pipeline; the desktop window is irrelevant.
//
// Modes span three aspect ratios on purpose: half-SBS always halves the
// horizontal density relative to vertical, and 16:9 wastes horizontal pixels
// on a headset that needs ~1.25:1 per eye, so a 4:3 / 5:4 total frame reaches
// the same sharpness with far fewer pixels.
// blocking=false is for the DXGI export wrappers: they must never wait (they can
// be entered under the loader lock from another injected DLL). Device-free.
bool InstallResolutionUnlock(bool blocking = true);

// Present thread, once per frame. ~10 s after start, compares the display mode the game settled on (its own log's
// "Display mode: WxH") with vrmod_fakedesktop.txt, and warns loudly (log + failure tone) if they differ: the game then
// renders at the smaller size and the result is stretched to the swapchain (2026-09-25: 4096x2160 into 6368x4592).
void ResolutionCheckOnPresent();
// The game's own display mode, read from its log by the check (~10 s after the first frame); false until then.
bool ResolutionGameMode(unsigned* w, unsigned* h);

// Swapchain/factory hooks; needs a throwaway swapchain, so init thread only.
bool InstallSwapChainWatchLate();

// The game's window swap chain is HDR (10-bit or floating-point; its HDR option on) -- the headset expects the standard
// 8-bit image, and the overlay says so in play (1.0.2).
bool GameSwapChainHdr();

// Real system CreateDXGIFactory1 (not our own export). Defined in dxgi_exports.cpp.
HRESULT RealCreateDXGIFactory1(REFIID riid, void** ppFactory);

} // namespace vrmod
