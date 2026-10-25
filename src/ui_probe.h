#pragma once

#include <d3d12.h>
#include <cstdint>
#include <string>

namespace vrmod {

// Brief 2, phase H0 (docs/HUD.md): identify the HUD's draws. Installed only with [debug] ui_probe=1 (hooks
// D3D12CreateDevice at startup, then the device's PSO/RTV creation and the command-list/queue calls). Ctrl+Alt+U
// captures 3 frames of command lists to vrmod_uiprobe_<time>.txt.
bool InstallUiProbe();      // dllmain, before the game creates its device
void UiProbeOnPresent();    // Present thread, once per frame
// The HUD placement ([hud] enabled / scale / offset_y), live (in-headset overlay). Get returns false when the HUD hooks
// aren't installed ([hud] enabled=0 at startup: switching it on needs a restart).
bool UiProbeHudGet(bool* on, float* scale, float* offsetY);
void UiProbeHudSet(bool on, float scale, float offsetY);

// The queue's ExecuteCommandLists is hooked here when the HUD/probe hooks are installed, so another module can't hook
// it again (MinHook: already created). Such a module registers a filter instead: called for every submit with the
// original function; returns true when it has made the call itself.
using UiProbeExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using UiProbeExecuteFilter = bool (*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*, UiProbeExecuteFn original);
bool UiProbeSetExecuteFilter(UiProbeExecuteFilter filter);   // false if the hook isn't installed

// Shifts the HUD (after its scaling) by an offset in the UI layer's normalised device coordinates (x right, y up; 1 =
// half the width / height). Used while aiming with head aim off, so the crosshair (the HUD centre) shows where the game
// really aims. 0, 0 = no shift.
void UiProbeSetAimOffset(float ndcX, float ndcY);

// Hiding in first person (see ui_probe.cpp): Lara's head model, the bow, the quiver, and (test) one body-model part
// (0 = none, 1..5). Set every frame by the camera hook. Stats since the last call, for the log.
void UiProbeSetHideGroups(bool head, bool bow, bool quiver, bool body = false);
// Gear finder (Ctrl+Alt+F6): first press lists the models drawn around Lara's head, each further press hides the next one
// (wrapping to off). Returns the log line. UiProbeGearTestIndex: the candidate hidden now, 0 = none.
std::string UiProbeGearTestStep();
int UiProbeGearTestIndex();
std::string UiProbeHideStats();
// A pipeline of ours with the UI signature (hud_layer.cpp's composite): never treated as a game UI pipeline.
void UiProbeMarkNotUi(const void* pso);

}
