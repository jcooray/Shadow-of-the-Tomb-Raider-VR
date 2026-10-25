#pragma once

struct ID3D12Device;

namespace vrmod {

// Opt-in via [debug] d3d12_debug_layer=1 in vrmod_settings.ini.
// Must run before the game creates its D3D12 device: enabling the debug layer afterwards makes the runtime
// remove the device. Needs the Windows "Graphics Tools" optional feature (d3d12SDKLayers.dll).
void D3D12DebugEnableIfRequested();

// Present thread, once per frame: moves warnings/errors from the device's info queue into vrmod.log
// (first few of each message ID; counts for the rest). Messages come from every user of the device --
// the game, OptiScaler/DLSS, the OpenXR runtime, and us -- so each one needs attributing before acting on it.
void D3D12DebugDrain(ID3D12Device* device);

}
