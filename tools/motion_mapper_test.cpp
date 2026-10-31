// Offline check of the motion controllers' gamepad mapping (src/motion_input.cpp's Mapper), above all the Touch layout,
// which can't be tried without Quest controllers. Feeds scripted controller input at 90 Hz and checks the gamepad
// state. Usage: vrmod_motion_mapper_test (exit code 0 = all passed).
#include "../src/motion_input.cpp"

#include <cstdio>

namespace modlog {
void Line(const std::string& message) { printf("log: %s\n", message.c_str()); }
}

namespace vrmod {
void* EyeGazeCreateActions(void*, void*, uint64_t) { return nullptr; }
void EyeGazeActionsAttached() {}
void EyeGazeUpdate(int64_t, int) {}
float SettingGet(const char*) { return 1.0f; }
}

namespace {

using vrmod::Input;
using vrmod::Mapper;

int g_failed = 0;
ULONGLONG g_now = 1000;

void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_failed;
}

// Runs the mapper for ms milliseconds with the same input; returns every button seen and the last output.
struct Run {
    WORD seen = 0;
    bool chord = false;
    Mapper::Out last;
};
Run Hold(Mapper& m, const Input& in, int ms) {
    Run r;
    for (int t = 0; t < ms; t += 11) {
        r.last = m.Update(in, g_now);
        r.seen |= r.last.pad.wButtons;
        r.chord |= r.last.chord;
        g_now += 11;
    }
    return r;
}

Input TouchIdle() {
    Input in;
    in.active = true;
    in.hasMenu = true;
    return in;
}

Input IndexIdle() {
    Input in;
    in.active = true;
    in.hasPads = true;
    return in;
}

}  // namespace

int main() {
    {   // Touch: a short right stick click is the stick click (sent on release).
        Mapper m;
        Input in = TouchIdle();
        Hold(m, in, 50);
        in.rclick = true;
        const Run down = Hold(m, in, 150);
        in.rclick = false;
        const Run up = Hold(m, in, 200);
        Check(!(down.seen & XINPUT_GAMEPAD_RIGHT_THUMB) && (up.seen & XINPUT_GAMEPAD_RIGHT_THUMB), "Touch: short right stick click -> RS on release");
    }
    {   // Touch: held without a direction becomes the held stick click, and the left stick walks again.
        Mapper m;
        Input in = TouchIdle();
        in.rclick = true;
        const Run early = Hold(m, in, 300);
        Hold(m, in, 100);
        in.ly = 0.8f;
        const Run held = Hold(m, in, 300);
        Check(!(early.seen & XINPUT_GAMEPAD_RIGHT_THUMB) && (held.last.pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB),
              "Touch: right stick click held 0.35 s -> RS held");
        Check(held.last.pad.sThumbLY > 20000 && !(held.seen & XINPUT_GAMEPAD_DPAD_UP), "Touch: ... and the left stick walks, no D-pad");
    }
    {   // Touch: a right stick click while walking is the stick click at once, and walking goes on.
        Mapper m;
        Input in = TouchIdle();
        in.ly = 0.9f;
        Hold(m, in, 100);
        in.rclick = true;
        const Run w = Hold(m, in, 100);
        Check((w.last.pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) && w.last.pad.sThumbLY > 20000 && !(w.seen & 0x000F),
              "Touch: right stick click while walking -> RS, still walking, no D-pad");
    }
    {   // Touch: right stick click + left stick up = D-pad up; no walking, no RS.
        Mapper m;
        Input in = TouchIdle();
        in.rclick = true;
        Hold(m, in, 60);
        in.ly = 0.9f;
        const Run d = Hold(m, in, 200);
        in.ly = 0;
        Hold(m, in, 50);
        in.rclick = false;
        const Run after = Hold(m, in, 200);
        Check((d.last.pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) && d.last.pad.sThumbLY == 0, "Touch: right stick click + left stick up -> D-pad up, no walking");
        Check(!((d.seen | after.seen) & XINPUT_GAMEPAD_RIGHT_THUMB), "Touch: ... and no RS");
    }
    {   // Touch: the other three directions.
        const struct { float x, y; WORD want; const char* name; } dirs[] = {
            { 0, -0.9f, XINPUT_GAMEPAD_DPAD_DOWN, "Touch: D-pad down" },
            { -0.9f, 0, XINPUT_GAMEPAD_DPAD_LEFT, "Touch: D-pad left" },
            { 0.9f, 0, XINPUT_GAMEPAD_DPAD_RIGHT, "Touch: D-pad right" },
        };
        for (const auto& d : dirs) {
            Mapper m;
            Input in = TouchIdle();
            in.rclick = true;
            Hold(m, in, 60);   // click first, then the direction
            in.lx = d.x;
            in.ly = d.y;
            Check((Hold(m, in, 150).last.pad.wButtons & d.want) != 0, d.name);
        }
    }
    {   // Touch: Menu tap = Menu (Start) on release; held 1.5 s = View, no Menu.
        Mapper m;
        Input in = TouchIdle();
        in.menu = true;
        const Run tapDown = Hold(m, in, 200);
        in.menu = false;
        const Run tapUp = Hold(m, in, 200);
        Check(!(tapDown.seen & XINPUT_GAMEPAD_START) && (tapUp.seen & XINPUT_GAMEPAD_START), "Touch: Menu tap -> Menu");
        in.menu = true;
        const Run hold = Hold(m, in, 1700);
        in.menu = false;
        const Run holdUp = Hold(m, in, 200);
        Check((hold.seen & XINPUT_GAMEPAD_BACK) && !((hold.seen | holdUp.seen) & XINPUT_GAMEPAD_START), "Touch: Menu held 1.5 s -> View, no Menu");
    }
    {   // Touch: Menu + right stick click = the overlay chord, and neither Menu nor RS afterwards.
        Mapper m;
        Input in = TouchIdle();
        in.menu = true;
        Hold(m, in, 100);
        in.rclick = true;
        const Run c = Hold(m, in, 200);
        in.menu = in.rclick = false;
        const Run after = Hold(m, in, 300);
        Check(c.chord, "Touch: Menu + right stick click -> overlay chord");
        Check(!((c.seen | after.seen) & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_BACK)),
              "Touch: ... and no Menu, RS or View from it");
    }
    {   // Index: a swipe up = one D-pad up; a slow drift = nothing; pressing = View, no D-pad.
        Mapper m;
        Input in = IndexIdle();
        in.padTouch = true;
        in.padY = -0.4f;
        Hold(m, in, 50);
        Run swipe;
        for (int i = 0; i < 5; ++i) { in.padY += 0.2f; const Run r = Hold(m, in, 33); swipe.seen |= r.seen; }
        in.padTouch = false;
        const Run after = Hold(m, in, 300);
        Check((swipe.seen | after.seen) & XINPUT_GAMEPAD_DPAD_UP, "Index: swipe up -> D-pad up");
        Mapper m2;
        Input slow = IndexIdle();
        slow.padTouch = true;
        slow.padX = -0.5f;
        WORD drift = 0;
        for (int i = 0; i < 10; ++i) { slow.padX += 0.1f; drift |= Hold(m2, slow, 200).seen; }
        Check(!(drift & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)), "Index: slow drift -> no D-pad");
        Mapper m3;
        Input press = IndexIdle();
        press.padTouch = true;
        press.lPadForce = 0.8f;
        const Run p = Hold(m3, press, 300);
        Check((p.last.pad.wButtons & XINPUT_GAMEPAD_BACK) && !(p.seen & 0x000F), "Index: left trackpad press -> View, no D-pad");
    }
    {   // Index: both trackpads = the overlay chord, nothing else.
        Mapper m;
        Input in = IndexIdle();
        in.lPadForce = in.rPadForce = 0.8f;
        const Run c = Hold(m, in, 300);
        in.lPadForce = in.rPadForce = 0;
        const Run after = Hold(m, in, 300);
        Check(c.chord && !((c.seen | after.seen) & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)), "Index: both trackpads -> overlay chord only");
    }
    {   // Grips: a light hold isn't a shoulder button; a squeeze is.
        Mapper m;
        Input in = TouchIdle();
        in.lg = 0.3f;
        const Run light = Hold(m, in, 100);
        in.lg = 0.8f;
        const Run hard = Hold(m, in, 100);
        Check(!(light.seen & XINPUT_GAMEPAD_LEFT_SHOULDER) && (hard.last.pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER), "Grip: squeeze -> LB, light hold -> nothing");
    }
    printf("%s (%d failed)\n", g_failed ? "FAILED" : "all passed", g_failed);
    return g_failed ? 1 : 0;
}
