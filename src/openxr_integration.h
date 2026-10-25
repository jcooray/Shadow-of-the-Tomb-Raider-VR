#pragma once

#include <cstdint>

struct IDXGISwapChain;
struct IUnknown;

namespace vrmod {

// Lazily initializes an OpenXR session on first call (bound to the game's
// D3D11 device, obtained from the swap chain), then submits the current
// left/right eye textures from eye_split.cpp as a stereo projection layer.
// Call once per frame from hkPresent, after UpdateEyeSplitTextures. Safe
// no-op if OpenXR init fails (e.g. no runtime/headset) or hasn't completed;
// failures are logged once, not retried every frame.
//
// User's setup: Pimax Dream Air via SteamVR (Sboys3 native driver) with
// parallel projection enabled, acting as the OpenXR runtime.
void UpdateOpenXR(IDXGISwapChain* swapChain);

// Destroys the OpenXR session/instance and stops all further XR work. Safe to
// call from any thread, more than once; gives up (logged) rather than hang if
// a frame is stuck inside an XR call and won't release the lock.
void ShutdownOpenXR();

// Called from the swapchain-creation hooks with the first argument the game
// passed (a D3D11 device, or for D3D12 the command queue). Remembers the queue
// so the D3D12 path can bind the OpenXR session to it. Ignores D3D11 devices.
void OpenXRNoteSwapChainDevice(IUnknown* deviceOrQueue);

// Pipeline settings the pairing test (Ctrl+P) varies, and restores afterwards:
//   framesAhead : GPU frames the CPU may run ahead before our copy waits (0 = no limit; key ' cycles 0..3)
//   halfRate    : the game renders every other display slot (End toggles)
struct XrPipelineConfig {
    int framesAhead = 1;
    bool halfRate = false;
};
XrPipelineConfig OpenXRGetPipelineConfig();
void OpenXRSetPipelineConfig(const XrPipelineConfig& config);

// Phase 6 projection probe helpers (Present thread):
//   the FOV model in use (0..3, see ComputeHalfTans), the half-angle tangents a FOV value gives under a model, and
//   the FOV value currently written into the game camera in AER mode (0 = not set).
int OpenXRFovModel();
void OpenXRPredictFrustum(float fov, int model, float* tanH, float* tanV);
float OpenXRAerGameFov();

} // namespace vrmod
