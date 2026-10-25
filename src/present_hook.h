#pragma once

#include <mutex>

struct IDXGISwapChain;

namespace vrmod {

// Creates a throwaway D3D11 device/swapchain purely to read IDXGISwapChain's vtable,
// then uses MinHook to detour Present(). Safe to call from the init thread only
// (must not be called from DllMain -- LoadLibrary/device creation inside DllMain
// deadlocks on the loader lock).
bool InstallPresentHook();
void RemovePresentHook();

// Initializes MinHook exactly once, whichever thread gets there first: the
// init thread, or the game's own thread via the DXGI export wrappers (which now
// install the resolution hooks synchronously on the game's first
// CreateDXGIFactory* call so they beat its first display-mode enumeration).
bool EnsureMinHookInitialized();

// Every throwaway D3D11 device/swapchain we create just to read vtables must take
// this lock: two threads creating devices at once (the init thread and the game's
// own thread inside the DXGI export wrappers) deadlocked as soon as another
// injected DXGI layer (OptiScaler) was present. Recursive: the same thread can
// re-enter through the export wrappers.
std::recursive_mutex& ProbeDeviceMutex();

// Logs this process's dedicated-VRAM usage and the OS budget for the swapchain's
// adapter (IDXGIAdapter3::QueryVideoMemoryInfo). Used around resizes to see how
// close huge resolutions get to the card's limit.
void LogVideoMemory(IDXGISwapChain* swapChain, const char* tag);

} // namespace vrmod
