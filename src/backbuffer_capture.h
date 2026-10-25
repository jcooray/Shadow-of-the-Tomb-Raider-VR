#pragma once

struct IDXGISwapChain;

namespace vrmod {

// Press F5 in-game to capture the current backbuffer -- however the game is
// actually packing it (SBS squeeze, full double-width, mono, whatever) -- to
// a BMP file next to the DLL, so we can visually inspect the real eye-image
// layout before building a real per-frame split into two eye textures.
void CaptureBackbufferIfHotkeyPressed(IDXGISwapChain* swapChain);

} // namespace vrmod
