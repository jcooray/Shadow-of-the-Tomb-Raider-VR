#pragma once

#include <cstdint>

namespace vrmod {

// Gamepad for the in-headset overlay (vr_overlay.cpp, docs/OVERLAY.md).
//
// Reading: XInputGetStateEx (xinput1_4 ordinal 100), which unlike XInputGetState reports the Guide (Xbox) button,
// every connected pad combined -- the same pad can sit in any slot (a virtual pad may hold slot 0).
// Blocking: the game's XInputGetState is hooked in each XInput DLL it has loaded. While blocked, the game sees a
// connected pad with nothing pressed (the overlay is open, or a button pressed in it is still held). Start is always
// hidden from the game while Guide is held on the same pad, so Menu + Guide never opens the game's pause menu.
struct PadState {
    bool connected = false;
    uint16_t buttons = 0;   // XINPUT_GAMEPAD_* bits, plus kPadGuide
    int16_t lx = 0, ly = 0;
    uint8_t lt = 0, rt = 0;
};
constexpr uint16_t kPadGuide = 0x0400;

void OverlayPadPoll();                    // Present thread, once per frame: hooks the game's XInput DLLs as they load
bool OverlayPadRead(PadState* out);       // the pads combined; false if none is connected
void OverlayPadBlockGame(bool block);
uint64_t OverlayPadGameReads();           // the game's XInputGetState calls so far (log: does it read XInput at all?)

}  // namespace vrmod
