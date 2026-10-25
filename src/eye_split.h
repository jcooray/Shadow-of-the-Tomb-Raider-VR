#pragma once

struct IDXGISwapChain;
struct ID3D11Texture2D;

namespace vrmod {

// SOTTR's native DX11 Side-by-Side stereo mode packs both eyes, each already
// rendered and DLSS-upscaled independently at full quality, into one
// backbuffer: left eye at X [0, W/2), right eye at X [W/2, W), both full
// height. Confirmed visually via BackbufferCapture -- see project memory.
//
// Splits that backbuffer into two persistent per-eye GPU textures every
// frame via CopySubresourceRegion, ready to hand to OpenXR once that
// integration exists. Call once per frame from hkPresent. Recreates the eye
// textures automatically if the backbuffer's dimensions/format change (e.g.
// the user changes resolution).
void UpdateEyeSplitTextures(IDXGISwapChain* swapChain);

// Press F4 in-game to dump the current left/right eye textures to
// eye_left.bmp / eye_right.bmp next to the DLL -- lets us visually confirm
// the split crop lands on the right halves (not swapped/offset) before any
// OpenXR code depends on it.
void DumpEyeTexturesIfHotkeyPressed();

// Current eye textures, or nullptr if UpdateEyeSplitTextures hasn't created
// them yet. Owned by eye_split.cpp -- don't Release() these.
ID3D11Texture2D* GetLeftEyeTexture();
ID3D11Texture2D* GetRightEyeTexture();

} // namespace vrmod
