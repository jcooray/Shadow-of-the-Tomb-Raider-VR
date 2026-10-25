#pragma once

#include <windows.h>
#include <thread>

namespace vrmod {

// Short audible confirmation for toggles, so state changes can be heard from inside the headset
// without reading the log. Plays on a detached thread so it never blocks the render thread.
// Windows' Beep goes to the default audio output device.
enum class Cue {
    On,        // rising two-tone
    Off,       // single low tone
    Success,   // three rising tones
    Failure,   // three falling tones (Success in reverse), then two low pulses
};

inline void PlayCue(Cue cue) {
    std::thread([cue] {
        switch (cue) {
        case Cue::On:      Beep(880, 90);  Beep(1320, 120); break;
        case Cue::Off:     Beep(440, 160); break;
        case Cue::Success: Beep(660, 80);  Beep(880, 80);  Beep(1320, 140); break;
        case Cue::Failure: Beep(1320, 80); Beep(880, 80); Beep(660, 140); Sleep(80); Beep(330, 110); Sleep(60); Beep(330, 110); break;
        }
    }).detach();
}

} // namespace vrmod
