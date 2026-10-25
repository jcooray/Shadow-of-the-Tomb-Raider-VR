#pragma once

#include "build_config.h"

namespace vrmod {

// D3D12 counterpart to the D3D11 constant buffer dumper in present_hook.cpp.
// Hooks ID3D12Resource::Map/Unmap; tap F9 in-game to dump a buffer's contents
// to vrmod.log whenever Map() is called on it while F9 is held.
//
// Caveat vs. the D3D11 version: D3D12 engines often map an upload-heap buffer
// ONCE persistently and just memcpy new data into the same CPU pointer every
// frame without calling Map again, in which case this hook will only catch
// the buffer's *first* frame of data, not subsequent updates. If mapCalls
// stays low after startup, that's what's happening, and we'll need to hook
// ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView instead (the
// per-draw-call binding point, which fires every frame regardless of mapping
// strategy).
#if VRMOD_DIAG
bool InstallD3D12ConstantBufferHook();
#else
inline bool InstallD3D12ConstantBufferHook() { return true; }
#endif

} // namespace vrmod
