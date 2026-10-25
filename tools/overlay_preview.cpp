// Offline preview of the in-headset overlay (src/vr_overlay.cpp): drives it with a scripted gamepad and writes each page
// as a BMP, with the game-side functions it calls replaced by stand-ins. Nothing of the game runs.
// Usage: vrmod_overlay_preview <output folder>   (reads / writes vrmod_settings.ini in the current folder)
#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <string>

#include "../src/aer.h"
#include "../src/afw.h"
#include "../src/bone_scan.h"
#include "../src/camera_interceptor.h"
#include "../src/cinematic_watch.h"
#include "../src/dlss_probe.h"
#include "../src/eye_gaze.h"
#include "../src/game_addresses.h"
#include "../src/head_drive.h"
#include "../src/head_tracking.h"
#include "../src/hud_layer.h"
#include "../src/logger.h"
#include "../src/openvr_gaze.h"
#include "../src/overlay_pad.h"
#include "../src/resolution_unlock.h"
#include "../src/settings.h"
#include "../src/ui_probe.h"
#include "../src/vr_overlay.h"

namespace modlog {
void Line(const std::string& message) { printf("log: %s\n", message.c_str()); }
}

namespace {
uint16_t g_buttons = 0;
bool g_fp = true, g_aim = true, g_drive = true, g_stab = true, g_hide = true, g_afw = true, g_hudOn = true, g_hudLayer = true;
float g_eye = 6.0f, g_hudScale = 0.6f, g_hudOff = 0.0f;
}

namespace vrmod {
void OverlayPadPoll() {}
bool OverlayPadRead(PadState* out) { *out = PadState{}; out->connected = true; out->buttons = g_buttons; return true; }
void OverlayPadBlockGame(bool) {}
uint64_t OverlayPadGameReads() { return 1234; }
bool AfwEnabled() { return g_afw; }
void AfwSetEnabled(bool on) { g_afw = on; }
bool AfwCantedEyes() { return false; }
int DlssPerEyeState() { return 0; }
bool DlssPerEyeSet(bool) { return true; }
void GameAddressSummary(int* f, int* t, int* ef, int* et, bool* k) { *f = 16; *t = 16; *ef = 32; *et = 32; *k = true; }
bool IsFirstPersonEnabled() { return g_fp; }
bool MenuScreenActive() { return false; }
bool CinematicActive() { return false; }
bool IsHeadTrackingEnabled() { return true; }
bool EyeGazeLatest(float*, double) { return false; }
bool OpenVrGazeLatest(int, float*, double, uint64_t*) { return true; }
void RequestFirstPersonToggle() { g_fp = !g_fp; }
bool HeadAimEnabled() { return g_aim; }
void SetHeadAimEnabled(bool on, const char*) { g_aim = on; }
bool HeadDriveEnabled() { return g_drive; }
void HeadDriveToggle(const char*) { g_drive = !g_drive; }
float HeadBoneEyeForwardCm() { return g_eye; }
void HeadBoneAdjustEye(float f, float, float) { g_eye += f; }
bool HeadBoneStabiliseOn() { return g_stab; }
float HeadBoneStandingHeightCm(uint32_t* n) { if (n) *n = 5000; return 152.5f; }
bool ResolutionGameMode(unsigned* w, unsigned* h) { *w = 5120; *h = 2880; return true; }
void HeadBoneToggleStabilise() { g_stab = !g_stab; }
bool HideHeadEnabled() { return g_hide; }
void SetHideHeadEnabled(bool on, const char*) { g_hide = on; }
void AerSetWorldScale(float) {}
void AerRequestRecenter(const char*) {}
void HeadTrackingRequestRecenter() {}
bool UiProbeHudGet(bool* on, float* s, float* o) { *on = g_hudOn; *s = g_hudScale; *o = g_hudOff; return true; }
void UiProbeHudSet(bool on, float s, float o) { g_hudOn = on; g_hudScale = s; g_hudOff = o; }
bool HudLayerEnabled() { return g_hudLayer; }
void HudLayerSetEnabled(bool on) { g_hudLayer = on; }
void EventMarkNow(const char*) {}
}  // namespace vrmod

namespace {

void WriteBmp(const std::string& path, const uint8_t* bgra, int w, int h) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + w * h * 4;
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (!f) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    // Composite over grey so the rounded corners show.
    for (int i = 0; i < w * h; ++i) {
        const uint8_t* p = bgra + i * 4;
        const float a = p[3] / 255.0f;
        uint8_t o[4];
        for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>(p[c] * a + 90 * (1.0f - a));
        o[3] = 255;
        fwrite(o, 4, 1, f);
    }
    fclose(f);
}

void Tick(uint16_t buttons, int times = 1) {
    for (int i = 0; i < times; ++i) {
        g_buttons = buttons;
        vrmod::VrOverlayTick();
        Sleep(2);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    if (argc > 2 && std::string(argv[2]) == "firstrun") {   // writes vrmod_settings.ini in the current folder if it's missing
        printf("first run: %d\n", vrmod::SettingsFirstRun() ? 1 : 0);
        return 0;
    }
    vrmod::OverlayXrInfo xr;
    xr.runtime = "SteamVR/OpenXR 2.18.4";
    xr.system = "Pimax Dream Air";
    xr.refreshHz = 72.0f;
    xr.rates = { 60.0f, 72.0f, 90.0f };
    xr.eyeW = 3184;
    xr.eyeH = 4592;
    vrmod::VrOverlaySetXrInfo(xr);
    Tick(0, 3);
    vrmod::VrOverlayNotify("Restart the game once to render at 6368 x 4592 (150 % render resolution). It runs at 5120 x 2880 now.", 5);
    Tick(0, 2);
    {
        int w = 0, h = 0;
        if (const uint8_t* img = vrmod::VrOverlayTakeImage(&w, &h)) WriteBmp(out + "\\overlay_toast.bmp", img, w, h);
    }
    vrmod::VrOverlayNotify("Auto settings detected: render resolution 150 % for your NVIDIA GeForce RTX 5090 and headset (with eye "
                           "tracked foveated rendering and DLSS 5). Change any setting in the overlay: hold Guide and press Menu.", 5);
    Tick(0, 2);
    {
        int w = 0, h = 0;
        if (const uint8_t* img = vrmod::VrOverlayTakeImage(&w, &h)) WriteBmp(out + "\\overlay_toast_auto.bmp", img, w, h);
    }
    vrmod::VrOverlayNotify("Your graphics card may not hold 72 Hz in this headset, even at the 50 % render resolution chosen for it. "
                           "Lower the game's graphics settings (DLSS Performance, a lower quality preset), and apply the "
                           "minimum-spec settings in this overlay (Guide + Menu, Rendering page).", 5);
    Tick(0, 2);
    {
        int w = 0, h = 0;
        if (const uint8_t* img = vrmod::VrOverlayTakeImage(&w, &h)) WriteBmp(out + "\\overlay_toast_gpu.bmp", img, w, h);
    }
    Tick(vrmod::kPadGuide | XINPUT_GAMEPAD_START);   // open
    Tick(0, 2);
    for (int page = 0; page < 6; ++page) {
        if (page == 1) { Tick(XINPUT_GAMEPAD_DPAD_DOWN); Tick(0); Tick(XINPUT_GAMEPAD_DPAD_DOWN); Tick(0); Tick(XINPUT_GAMEPAD_DPAD_DOWN); Tick(0); }
        Sleep(600);   // past the Status page's redraw limit
        Tick(0, 2);
        int w = 0, h = 0;
        const uint8_t* img = vrmod::VrOverlayTakeImage(&w, &h);
        if (img) WriteBmp(out + "\\overlay_page" + std::to_string(page) + ".bmp", img, w, h);
        else printf("page %d: no new image\n", page);
        Tick(XINPUT_GAMEPAD_RIGHT_SHOULDER);
        Tick(0);
    }
    // Confirmation test: from Status (the loop wrapped round) to Rendering, apply the minimum-spec preset (its first line),
    // RB -> the dialog (overlay_dialog.bmp); B undoes. Then the same again, closed with B and kept with A.
    auto grab = [&](const char* name) {
        Sleep(600);
        Tick(0, 2);
        int w = 0, h = 0;
        if (const uint8_t* img = vrmod::VrOverlayTakeImage(&w, &h)) WriteBmp(out + "\\" + name, img, w, h);
    };
    Tick(XINPUT_GAMEPAD_RIGHT_SHOULDER); Tick(0);
    Tick(XINPUT_GAMEPAD_RIGHT_SHOULDER); Tick(0);
    Tick(XINPUT_GAMEPAD_A); Tick(0);
    grab("overlay_preset.bmp");
    Tick(XINPUT_GAMEPAD_RIGHT_SHOULDER); Tick(0);
    grab("overlay_dialog.bmp");
    Tick(XINPUT_GAMEPAD_B); Tick(0);
    printf("undone\n");
    CopyFileA("vrmod_settings.ini", "after_undo.ini", FALSE);
    Tick(XINPUT_GAMEPAD_LEFT_SHOULDER); Tick(0);   // back to Rendering
    Tick(XINPUT_GAMEPAD_A); Tick(0);
    Tick(XINPUT_GAMEPAD_B); Tick(0);              // close -> dialog
    Tick(XINPUT_GAMEPAD_A); Tick(0);              // keep (and close)
    // The View page's last lines, for the world scale percentage.
    Tick(vrmod::kPadGuide | XINPUT_GAMEPAD_START); Tick(0);
    Tick(0, 2);
    Tick(XINPUT_GAMEPAD_LEFT_SHOULDER); Tick(0);
    for (int i = 0; i < 14; ++i) { Tick(XINPUT_GAMEPAD_DPAD_DOWN); Tick(0); }
    grab("overlay_view_bottom.bmp");
    // The Rendering page with DLSS 5 selected (its help shown).
    Tick(XINPUT_GAMEPAD_RIGHT_SHOULDER); Tick(0);
    for (int i = 0; i < 5; ++i) { Tick(XINPUT_GAMEPAD_DPAD_DOWN); Tick(0); }
    grab("overlay_rendering_dlss5.bmp");
    // Closed, then 3 s of play: the first-play prompt opens the overlay at World size (once per install).
    Tick(XINPUT_GAMEPAD_B); Tick(0);
    for (int i = 0; i < 40; ++i) { Sleep(100); Tick(0); }
    grab("overlay_first_world.bmp");
    Tick(XINPUT_GAMEPAD_A); Tick(0);   // A dismisses the notice; the panel stays open
    grab("overlay_first_world_dismissed.bmp");
    printf("done\n");
    return 0;
}
