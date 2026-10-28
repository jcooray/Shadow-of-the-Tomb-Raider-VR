#include "vr_overlay.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "aer.h"
#include "afw.h"
#include "bone_scan.h"
#include "build_config.h"
#include "camera_interceptor.h"
#include "cinematic_watch.h"
#include "dlss_probe.h"
#include "eye_gaze.h"
#include "game_addresses.h"
#include "gpu_tier.h"
#include "head_drive.h"
#include "head_tracking.h"
#include "hud_layer.h"
#include "logger.h"
#include "openvr_gaze.h"
#include "overlay_image_view_toggle.h"
#include "overlay_pad.h"
#include "resolution_unlock.h"
#include "settings.h"
#include "steamvr_settings.h"
#include "ui_probe.h"
#include "version.h"

namespace vrmod {

namespace {

// ---------------------------------------------------------------------------------------------------------------
// State

constexpr int kW = 1280, kH = 800;            // panel bitmap; shown overlay.width_m wide
constexpr int kListTop = 150, kRowH = 44, kListBottom = 650;
constexpr COLORREF kBg = RGB(24, 26, 31), kBar = RGB(34, 37, 44), kText = RGB(228, 230, 234), kDim = RGB(140, 146, 156),
                   kAccent = RGB(80, 150, 255), kSelBg = RGB(44, 70, 120), kOn = RGB(90, 205, 120), kOff = RGB(200, 120, 110),
                   kWarn = RGB(240, 180, 60), kKeys = RGB(235, 70, 70);   // kKeys: the panel's own keys, top right

enum class Kind { Info, Toggle, Number, Choice, Action };

struct Item {
    Kind kind = Kind::Info;
    std::string label, help;
    std::function<std::string()> value;      // shown on the right
    std::function<void(int)> change;         // left / right (-1 / +1); toggles flip on either
    std::function<void()> activate;          // A (toggles: flip; actions: run)
    std::function<void()> reset;             // Y: back to the default
    std::function<bool()> enabled;           // false: greyed out, can't change (with the reason in the value)
    bool restart = false;                    // applies after a restart (label marked *)
    // Instead of restart, for items where only some changes need one: true when going from the first shown value to the
    // second needs a restart (foveated rendering: only off <-> on).
    std::function<bool(const std::string&, const std::string&)> restartFor;
    bool preset = false;                     // an action that changes the page's other items (all count as changed)
    std::function<bool()> on;                // toggles: the current state (colour)
    // Snapshot for the page's confirmation: returns a function that puts the current state back (undo).
    std::function<std::function<void()>()> capture;
};

struct Page {
    const char* name;
    std::vector<Item> items;
};

std::vector<Page> g_pages;
int g_page = 0;
int g_sel[8] = {};
int g_scroll[8] = {};
bool g_open = false;
bool g_place = false;
bool g_releaseHold = false;          // closed, but a button pressed in the overlay is still down: keep the game blind
OverlayXrInfo g_xr;
float g_refreshRequest = 0.0f;
bool g_screenToggle = false;
std::string g_msg;                   // a short message in the help area (action results)
ULONGLONG g_msgUntil = 0;
std::string g_toast;                 // a message with the panel closed (VrOverlayNotify)
ULONGLONG g_toastUntil = 0;
std::vector<std::pair<std::string, std::string>> g_extraSave;   // keys outside the live table (HUD, eye offset, ...)

// Confirmation per page: a snapshot when the page is entered; leaving it (LB / RB / close) with changes asks to keep or
// undo them. Nothing is written to vrmod_settings.ini before "keep".
struct Snapshot {
    int page = -1;
    std::vector<std::string> values;              // each item's shown value at the snapshot
    std::vector<std::function<void()>> restore;   // per item (empty: not restorable)
    std::vector<bool> touched;                     // changed from the overlay (a value that moved by itself isn't a change)
    std::vector<float> settings;                   // every live setting (presets change keys no item shows)
} g_snap;
struct Confirm {
    bool active = false;
    int then = 0;   // after the answer: +1 / -1 next / previous page, 2 close
} g_confirm;

// A notice over the panel -- sections of a heading and a few lines, divided by a rule -- dismissed with A or B; the
// panel stays open.
struct NoticeSection {
    std::string heading, text;
    const uint32_t* image = nullptr;   // optional, left of the text: 0xAARRGGBB, not premultiplied
    int imageW = 0, imageH = 0;
};
struct Notice {
    bool active = false;
    bool alone = false;        // shown on its own (else drawn over the panel)
    bool opensPanel = false;   // alone: dismissing it opens the panel, on the page it was given (else it just closes)
    std::vector<NoticeSection> sections;
    std::string okText = "A: OK";
    std::function<void()> onOk;   // A / Enter runs this (then it is dismissed)
    bool canLater = false;        // with onOk: B dismisses it without running it ("B: later")
} g_notice;

void ShowAutoSettingsNotice(bool restartLater, float pct);   // StartupMessages (defined with the input below)

// The render resolution check (StartupMessages): done at least once, and whether the game runs at another size than the
// one set (a restart pending). First run: the restart notice is still to show once a level has loaded.
bool g_resChecked = false, g_restartPending = false, g_restartPrompt = false;

// Frame rate (ticks are presented frames).
double g_fps = 0.0, g_frameMsAvg = 0.0, g_frameMsMax = 0.0;

// Drawing
HDC g_dc = nullptr;
HBITMAP g_bmp = nullptr;
uint32_t* g_px = nullptr;
HFONT g_fTitle = nullptr, g_fTab = nullptr, g_fRow = nullptr, g_fHelp = nullptr, g_fKeys = nullptr;
std::vector<uint8_t> g_image;        // BGRA with alpha, handed to the OpenXR side
bool g_imageNew = false;
std::string g_lastSig;

std::string Fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof(b), f, v);
    return b;
}

std::wstring W(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void Message(const std::string& m, int ms = 4000) {
    g_msg = m;
    g_msgUntil = GetTickCount64() + static_cast<ULONGLONG>(ms);
}

void SaveSoon() {}   // changes wait for the page's confirmation (SaveNow on "keep")

void SaveExtra(const std::string& key, const std::string& value) {
    for (auto& kv : g_extraSave) if (kv.first == key) { kv.second = value; return; }
    g_extraSave.emplace_back(key, value);
}

void SaveNow() {
    const int n = SettingsSaveChanged();
    if (!g_extraSave.empty()) {
        SettingsWriteKeys(g_extraSave);
        g_extraSave.clear();
    }
    (void)n;
}

bool Aer() {
    return GetRenderMode() == RenderMode::Aer;
}

// ---------------------------------------------------------------------------------------------------------------
// Item builders

Item Info(const std::string& label, std::function<std::string()> value, const std::string& help = "") {
    Item it;
    it.kind = Kind::Info;
    it.label = label;
    it.value = std::move(value);
    it.help = help;
    return it;
}

Item Action(const std::string& label, const std::string& help, std::function<void()> run) {
    Item it;
    it.kind = Kind::Action;
    it.label = label;
    it.help = help;
    it.value = [] { return std::string("Press A"); };
    it.activate = std::move(run);
    return it;
}

// A toggle backed by a live settings key, optionally mirrored into a module (get / set) that keeps its own switch.
Item Toggle(const std::string& label, const std::string& help, const char* key, std::function<bool()> get = nullptr,
            std::function<void(bool)> set = nullptr, bool restart = false) {
    Item it;
    it.kind = Kind::Toggle;
    it.label = label;
    it.help = help;
    it.restart = restart;
    auto cur = [key, get] { return get ? get() : SettingGet(key) != 0.0f; };
    it.on = cur;
    it.value = [cur] { return std::string(cur() ? "On" : "Off"); };
    auto apply = [key, set](bool on) {
        if (key) SettingSet(key, on ? 1.0f : 0.0f);
        if (set) set(on);
        SaveSoon();
    };
    it.activate = [cur, apply] { apply(!cur()); };
    it.change = [cur, apply](int) { apply(!cur()); };
    if (key) it.reset = [key, apply] { apply(SettingDefault(key) != 0.0f); };
    it.capture = [cur, apply] {
        const bool v = cur();
        return std::function<void()>([v, cur, apply] { if (cur() != v) apply(v); });
    };
    return it;
}

// A number backed by a live settings key.
Item Number(const std::string& label, const std::string& help, const char* key, float step, std::function<std::string(float)> show,
            std::function<void(float)> set = nullptr, bool restart = false) {
    Item it;
    it.kind = Kind::Number;
    it.label = label;
    it.help = help;
    it.restart = restart;
    it.value = [key, show] { return show(SettingGet(key)); };
    auto apply = [key, set](float v) {
        SettingSet(key, v);
        if (set) set(SettingGet(key));
        SaveSoon();
    };
    it.change = [key, step, apply](int d) {
        const float v = SettingGet(key) + d * step;
        apply(std::round(v / step) * step);
    };
    it.reset = [key, apply] { apply(SettingDefault(key)); };
    it.capture = [key, apply] {
        const float v = SettingGet(key);
        return std::function<void()>([key, v, apply] { if (SettingGet(key) != v) apply(v); });
    };
    return it;
}

// A choice backed by a live settings key holding an index.
Item Choice(const std::string& label, const std::string& help, const char* key, std::vector<std::string> names, bool restart = false) {
    Item it;
    it.kind = Kind::Choice;
    it.label = label;
    it.help = help;
    it.restart = restart;
    const int n = static_cast<int>(names.size());
    it.value = [key, names] {
        const int i = static_cast<int>(SettingGet(key));
        return i >= 0 && i < static_cast<int>(names.size()) ? names[i] : std::string("?");
    };
    it.change = [key, n](int d) {
        int i = static_cast<int>(SettingGet(key)) + d;
        i = (i % n + n) % n;
        SettingSet(key, static_cast<float>(i));
        SaveSoon();
    };
    it.activate = [it]() mutable { it.change(1); };
    it.reset = [key] { SettingSet(key, SettingDefault(key)); SaveSoon(); };
    it.capture = [key] {
        const float v = SettingGet(key);
        return std::function<void()>([key, v] { SettingSet(key, v); });
    };
    return it;
}

std::string OffOr(float v, const char* fmt, const char* off) {
    return v <= 0.0f ? std::string(off) : Fmt(fmt, v);
}

// ---------------------------------------------------------------------------------------------------------------
// Pages

std::string ModVersion() {
    std::string v = std::string("Shadow of the Tomb Raider VR ") + SOTTRVR_VERSION_STRING + ", built " + __DATE__;
#if VRMOD_DIAG
    v += " (development build)";
#endif
    return v;
}

// SteamVR's own render resolution setting (steamvr_settings.h), as text, re-read every 10 s; empty when SteamVR's
// settings can't be read (another runtime). The mod takes it out of the size SteamVR recommends, so it doesn't change
// this game.
std::string SteamVrResolution() {
    static std::string text;
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= next) {
        next = now + 10000;
        const SteamVrScale r = ReadSteamVrScale();
        text = !r.read ? std::string()
                       : Fmt("%.0f %%", r.Total() * 100.0) + (r.manual ? " (manual" : " (Auto") + Fmt(" %.0f %%", r.global * 100.0)
                             + Fmt(", this game %.0f %%)", r.perApp);
    }
    return text;
}

// CheekyFoveatedDLSS's settings file (game folder relative, like the mod's own).
constexpr const char* kCheekyIni = "CheekyFoveatedDLSS\\CheekyFoveatedDLSS.ini";

// A key of its [CheekyFoveatedDLSS] section; false when it isn't there.
bool CheekyIniValue(const char* key, std::string* value) {
    std::ifstream in(kCheekyIni);
    const std::string prefix = std::string(key) + "=";
    std::string line;
    bool section = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') section = line == "[CheekyFoveatedDLSS]";
        else if (section && line.rfind(prefix, 0) == 0) {
            *value = line.substr(prefix.size());
            return true;
        }
    }
    return false;
}

// Its Logging key (Cheeky's default when the key is missing: on).
bool CheekyLoggingOn() {
    std::string v;
    if (CheekyIniValue("Logging", &v)) return v != "0";
    return GetFileAttributesA(kCheekyIni) != INVALID_FILE_ATTRIBUTES;
}

// The sharp region's size in Cheeky's ini (Width / Height, fractions of the image); the mode's default when not set.
void CheekyFoveaSize(float* w, float* h) {
    FoveaSize(CurrentFoveation(), w, h);
    std::string v;
    if (CheekyIniValue("Width", &v)) *w = static_cast<float>(atof(v.c_str()));
    if (CheekyIniValue("Height", &v)) *h = static_cast<float>(atof(v.c_str()));
}

void WriteCheekyFoveaSize(float w, float h) {
    IniWriteKeys(kCheekyIni, { { "CheekyFoveatedDLSS.Width", Fmt("%.2f", w) }, { "CheekyFoveatedDLSS.Height", Fmt("%.2f", h) } });
}

// Cheeky's ini follows the foveation settings (Cheeky reads it when it starts): on / off, DLSS 5 (eye tracked only),
// and on a change of mode the mode's own sharp region size.
void WriteCheekyFoveation(bool modeSize) {
    const Foveation f = CurrentFoveation();
    IniWriteKeys(kCheekyIni, { { "CheekyFoveatedDLSS.Enabled", f == Foveation::Off ? "0" : "1" },
                               { "CheekyFoveatedDLSS.NrEnabled", f == Foveation::EyeDlss5 ? "1" : "0" } });
    if (modeSize && f != Foveation::Off) {
        float w = 0.0f, h = 0.0f;
        FoveaSize(f, &w, &h);
        WriteCheekyFoveaSize(w, h);
    }
}

// The automatic render resolution (a new install's, until the player sets one) follows foveated rendering, which changes
// the DLSS pass's GPU time. Like any resolution change it applies at the next start.
void AutoResolutionFollow() {
    const float rel = SettingGet("render.gpu_relative"), tw = SettingGet("render.target_w"), th = SettingGet("render.target_h");
    if (SettingGet("render.resolution_auto") == 0.0f || rel <= 0.0f || tw < 640.0f) return;
    const Foveation f = CurrentFoveation();
    const uint32_t w = static_cast<uint32_t>(tw), h = static_cast<uint32_t>(th);
    const int pct = StartingResolutionPct(rel, w, h, f);
    SettingSet("render.resolution_pct", static_cast<float>(pct));
    SettingSet("render.gpu_note", PredictedFrameMs(rel, w, h, pct, f) > 1000.0 / 72.0 ? 1.0f : 0.0f);
    SettingSet("render.auto_foveation", static_cast<float>(f));
    modlog::Line(std::string("OVERLAY: foveated rendering ") + FoveationName(f) + " -> automatic render resolution " + std::to_string(pct) + " %");
}

std::string SaveBugReport() {
    wchar_t dll[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SaveBugReport), &self);
    GetModuleFileNameW(self, dll, MAX_PATH);
    std::wstring dir(dll);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\vrmod_report_%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring out = dir + name;
    if (!CreateDirectoryW(out.c_str(), nullptr)) return "could not create the report folder";
    SaveNow();
    int copied = 0;
    for (const wchar_t* f : { L"\\vrmod.log", L"\\vrmod.prev1.log", L"\\vrmod.prev2.log", L"\\vrmod_settings.ini" }) {
        if (CopyFileW((dir + f).c_str(), (out + f).c_str(), FALSE)) ++copied;
    }
    // Foveated rendering (CheekyFoveatedDLSS, if installed): its settings, and its logs when its logging is on.
    for (const wchar_t* f : { L"\\CheekyFoveatedDLSS.ini", L"\\CheekyFoveatedDLSS-Standalone.log", L"\\CheekyFoveatedDLSS-Host.log" }) {
        if (CopyFileW((dir + L"\\CheekyFoveatedDLSS" + f).c_str(), (out + f).c_str(), FALSE)) ++copied;
    }
    EventMarkNow("bug report folder saved");
    std::string folder;
    for (wchar_t c : std::wstring(name + 1)) folder += static_cast<char>(c);
    return "saved " + std::to_string(copied) + " files to the game folder, " + folder;
}

// The world scale of World size 100 % -- life size. The game's units are centimetres (the executable holds -980 / -981,
// gravity in cm/s^2; Lara herself falls at 900 and jumps at 720 units/s^2, tuned values), but at one game unit per real
// centimetre of head movement the world looks far too small in the headset -- the game seems built for its third-person
// camera. The developer found 1.8 times that true to life, standing and seated alike (2026-10-27), so 100 % is world_scale
// 1 / 1.8; it is also the default, as the opening cutscene (Lara in a ravine, unable to look down at herself) has to look
// right from the start. (Before: 1.0, and earlier still 0.87-0.91 from Lara's height, assuming 1.70 m.)
constexpr float kLifeSizeWorldScale = 1.0f / 1.8f;
float TrueWorldScale() {
    return kLifeSizeWorldScale;
}


// Render resolution as a percentage of the PIXELS of the headset's target size (SteamVR's convention: 100 % = full
// sharpness at the centre of the view, 150 % = 1.22 x each side).
std::string ResolutionText(float pct) {
    if (pct <= 0.0f) {
        // The game's own resolution (its log) -- not the headset image it is stretched to when the two differ.
        unsigned gw = 0, gh = 0;
        if (!ResolutionGameMode(&gw, &gh)) return "as set in the game";
        if (g_xr.eyeW && (gw != g_xr.eyeW || gh != g_xr.eyeH))   // short enough for the value column
            return "game " + std::to_string(gw) + " x " + std::to_string(gh) + ", STRETCHED";
        return "as set in the game (" + std::to_string(gw) + " x " + std::to_string(gh) + ")";
    }
    const float tw = SettingGet("render.target_w"), th = SettingGet("render.target_h");
    if (tw < 640.0f) return Fmt("%.0f %%", pct) + " (size known after the next start)";
    const float k = std::sqrt(pct / 100.0f);
    const int w = static_cast<int>(tw * k / 8.0f + 0.5f) * 8, h = static_cast<int>(th * k / 8.0f + 0.5f) * 8;
    // 100 % is SteamVR's own 100 % (its recommended size spread over the view the game renders): marked as such.
    // "auto": below 100 % because the mod chose it for the graphics card (a new install, until changed here).
    const char* note = std::fabs(pct - 100.0f) < 0.5f ? " = SteamVR" : SettingGet("render.resolution_auto") != 0.0f ? " auto" : "";
    return Fmt("%.0f %%", pct) + note + " (" + std::to_string(w) + " x " + std::to_string(h) + ")";
}

// Presets (Rendering page): set that page's items; the page's confirmation then lists the changes to keep or undo.
// Minimum spec: for GPUs well below the dev system (25-50 % of an RTX 5090) -- render at 80 % of the headset's target
// size, no overscan, no second DLSS history, frame warp on (each eye is shown at the full refresh rate from half the
// renders), the lowest headset refresh rate of at least 72 Hz.
void ApplyPreset(bool minimumSpec) {
    SettingSet("render.mode", 2.0f);
    // Minimum spec: 70 %, or the current value if it's lower already (e.g. the 50 % chosen for a slow card).
    const float cur = SettingGet("render.resolution_pct");
    SettingSet("render.resolution_pct", minimumSpec ? (cur >= 50.0f && cur < 70.0f ? cur : 70.0f) : 100.0f);
    SettingSet("render.resolution_auto", 0.0f);
    SettingSet("render.overscan", minimumSpec ? 1.0f : 1.08f);
    SettingSet("render.afw", 1.0f);
    AfwSetEnabled(true);
    SettingSet("render.dlss_per_eye", 1.0f);
    DlssPerEyeSet(true);
    SettingSet("render.aer_cutscenes", 0.0f);
    SettingSet("render.aer_predict_frames", 3.0f);
    if (minimumSpec && g_xr.rates.size() > 1) {
        float hz = 0.0f;
        for (float r : g_xr.rates) if (r >= 71.5f && (hz == 0.0f || r < hz)) hz = r;
        if (hz == 0.0f) hz = g_xr.rates.back();
        if (std::fabs(hz - g_xr.refreshHz) > 0.5f) {
            g_refreshRequest = hz;
            SettingSet("render.refresh_hz", hz);
        }
    }
    Message(minimumSpec ? "minimum-spec settings applied -- keep or undo when leaving this page"
                        : "standard settings applied -- keep or undo when leaving this page");
}

// One-off messages: welcome on the first run; render resolution not applied yet (a new install learns the headset's
// target size during its first start, and a changed SteamVR resolution changes it -- the game only takes a resolution
// at startup); once running at it, a graphics card expected to miss 72 Hz even at the automatic 50 % (render.gpu_note).
void StartupMessages() {
    static bool welcomed = false;
    static ULONGLONG since = 0;
    const ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    // First run: one message after another, each when the one before has gone -- the settings chosen for this PC, then
    // (below) the restart that applies the render resolution, then at the first play the World size notice and the
    // overlay (FirstWorldSizePrompt).
    static bool firstRun = false;   // the "Auto settings detected" notice still to show
    if (!welcomed && now - since > 15000 && g_xr.eyeW) {
        welcomed = true;
        firstRun = SettingsFirstRun();
    }
    // Eye tracked foveated rendering with no eye tracking: its small sharp region stays in the middle. Said once, after a
    // minute of play without any gaze (a headset without eye tracking, or its tracker not running).
    static ULONGLONG gazeCheck = 0, playSince = 0;
    static bool gazeDone = false;
    if (!gazeDone && now >= gazeCheck) {
        gazeCheck = now + 1000;
        float d[3];
        if (EyeGazeLatest(d, 1000.0) || OpenVrGazeLatest(0, d, 1000.0)) {
            gazeDone = true;
        } else if (CurrentFoveation() < Foveation::Eye || MenuScreenActive() || CinematicActive() || !IsHeadTrackingEnabled()) {
            playSince = 0;
        } else if (!playSince) {
            playSince = now;
        } else if (now - playSince > 60000) {
            gazeDone = true;
            VrOverlayNotify("No eye tracking reached the game, so foveated rendering keeps its small sharp area in the middle. "
                            "Without eye tracking, choose fixed or off in this overlay (Guide + Menu, Rendering page).", 15);
            modlog::Line("OVERLAY: eye tracked foveated rendering, but no gaze in a minute of play -- suggested fixed or off");
        }
    }
    static ULONGLONG nextCheck = 0;
    static std::string notified;
    if (now < nextCheck || now - since < 20000 || !g_xr.eyeW || GetRenderMode() == RenderMode::Sbs) return;
    nextCheck = now + 5000;
    const float pct = SettingGet("render.resolution_pct"), tw = SettingGet("render.target_w"), th = SettingGet("render.target_h");
    if (pct < 30.0f || tw < 640.0f) return;
    const float k = std::sqrt(pct / 100.0f);
    const unsigned w = static_cast<unsigned>(tw * k / 8.0f + 0.5f) * 8, h = static_cast<unsigned>(th * k / 8.0f + 0.5f) * 8;
    g_resChecked = true;
    g_restartPending = w != g_xr.eyeW || h != g_xr.eyeH;
    if (firstRun) {   // first run: the settings chosen for this PC; the restart that applies them once a level has loaded
        firstRun = false;
        notified = std::to_string(w) + "x" + std::to_string(h);
        g_restartPrompt = g_restartPending;
        ShowAutoSettingsNotice(g_restartPending, pct);
        return;
    }
    if (w == g_xr.eyeW && h == g_xr.eyeH) {
        if (SettingGet("render.gpu_note") != 0.0f) {
            SettingSet("render.gpu_note", 0.0f);
            SettingsWriteKeys({ { "render.gpu_note", "0" } });
            VrOverlayNotify("Your graphics card may not hold 72 Hz in this headset, even at the 50 % render resolution chosen for it. "
                            "Lower the game's graphics settings (DLSS Performance, a lower quality preset), and apply the "
                            "minimum-spec settings in this overlay (Guide + Menu, Rendering page).", 20);
            modlog::Line("OVERLAY: graphics card expected to miss 72 Hz at the chosen resolution -- suggested lower settings (once)");
        }
        return;
    }
    const std::string key = std::to_string(w) + "x" + std::to_string(h);
    if (notified == key || now < g_toastUntil) return;   // after the message before it
    notified = key;
    VrOverlayNotify("Restart the game once to render at " + std::to_string(w) + " x " + std::to_string(h) + " (" + Fmt("%.0f", pct)
                    + " % render resolution). It runs at " + std::to_string(g_xr.eyeW) + " x " + std::to_string(g_xr.eyeH) + " now.", 12);
    modlog::Line("OVERLAY: render resolution " + key + " applies at the next start (the game runs at " + std::to_string(g_xr.eyeW) + "x"
                 + std::to_string(g_xr.eyeH) + ")");
}

// The order a page shows its items in: these first, in this order, the rest after them as built. (Items are built in
// the order their code needs; this is the order players see.)
void ShowFirst(Page& p, const std::vector<std::string>& labels) {
    auto rank = [&labels](const Item& it) {
        return static_cast<size_t>(std::find(labels.begin(), labels.end(), it.label) - labels.begin());
    };
    std::stable_sort(p.items.begin(), p.items.end(), [&rank](const Item& a, const Item& b) { return rank(a) < rank(b); });
}

void BuildPages() {
    g_pages.clear();

    // --- Status
    {
        Page p{ "Status" };
        p.items.push_back(Info("Mod", [] { return ModVersion(); }));
        p.items.push_back(Info("Game", [] {
            int f = 0, t = 0, ef = 0, et = 0;
            bool known = false;
            GameAddressSummary(&f, &t, &ef, &et, &known);
            return std::string(known ? "supported build" : "DIFFERENT build") + ", addresses " + std::to_string(f) + "/" + std::to_string(t)
                + ", events " + std::to_string(ef) + "/" + std::to_string(et);
        }, "A different game build or missing addresses switch the features that need them off (see vrmod.log)."));
        p.items.push_back(Info("VR runtime", [] { return g_xr.runtime.empty() ? std::string("not started") : g_xr.runtime; }));
        p.items.push_back(Info("Headset", [] {
            std::string s = g_xr.system.empty() ? std::string("-") : g_xr.system;
            if (g_xr.refreshHz > 0) s += ", " + Fmt("%.0f Hz", g_xr.refreshHz);
            if (g_xr.eyeW) s += ", eye image " + std::to_string(g_xr.eyeW) + " x " + std::to_string(g_xr.eyeH);
            return s;
        }));
        p.items.push_back(Info("Render resolution", [] { return ResolutionText(SettingGet("render.resolution_pct")); },
                               "The mod's render resolution (Rendering page): the size the game renders each eye at."));
        p.items.push_back(Info("SteamVR's resolution setting", [] {
            const std::string t = SteamVrResolution();
            if (t.empty() || g_xr.runtime.find("SteamVR") == std::string::npos) return std::string("not in use (another VR runtime)");
            return t + ", not used here";
        }, "SteamVR's own resolution setting doesn't change this game: the mod takes it out and renders at its own Render "
           "resolution (Rendering page)."));
        p.items.push_back(Info("Rendering", [] {
            const RenderMode m = GetRenderMode();
            std::string s = m == RenderMode::Aer ? "alternate eye (AER)" : m == RenderMode::Mono ? "mono" : "side by side";
            if (m == RenderMode::Aer) s += AfwEnabled() ? " + frame warp" : "";
            const int d = DlssPerEyeState();
            s += d < 0 ? ", DLSS per eye unavailable" : d ? ", DLSS per eye" : "";
            return s;
        }));
        p.items.push_back(Info("Frame rate", [] {
            return Fmt("%.0f fps", g_fps) + Fmt(", frame %.1f ms", g_frameMsAvg) + Fmt(" (slowest %.0f ms)", g_frameMsMax);
        }, "Frames presented by the game per second, measured over the last second."));
        p.items.push_back(Info("View", [] {
            std::string s = IsFirstPersonEnabled() ? "first person" : "third person";
            if (MenuScreenActive()) s += ", menu open (head tracking paused)";
            else if (CinematicActive()) s += ", cutscene";
            else if (!IsHeadTrackingEnabled()) s += ", head tracking paused";
            return s;
        }));
        p.items.push_back(Info("Eye tracking", [] {
            float d[3];
            if (EyeGazeLatest(d, 250.0)) return std::string("gaze live (OpenXR)");
            if (OpenVrGazeLatest(0, d, 250.0)) return std::string("gaze live (SteamVR)");
            return std::string("no gaze");
        }, "Used for foveated rendering when available. Not needed to play."));
        p.items.push_back(Info("Gamepad", [] {
            PadState s;
            const bool c = OverlayPadRead(&s);
            return std::string(c ? "connected" : "none found") + (OverlayPadGameReads() ? ", game reads it through XInput" : ", game input not seen yet");
        }));
        p.items.push_back(Info("Settings", [] {
            return std::string("vrmod_settings.ini in the game folder");
        }, "Changes are saved when you keep them, as you leave a page or close the overlay."));
        g_pages.push_back(std::move(p));
    }

    // --- View
    {
        Page p{ "View" };
        Item fp = Toggle("First person", "First person view from Lara's eyes, or the game's third person camera.", nullptr,
                         [] { return IsFirstPersonEnabled(); }, [](bool) { RequestFirstPersonToggle(); });
        fp.reset = nullptr;
        p.items.push_back(fp);
        p.items.push_back(Action("Recentre the view", "Makes the direction you face now straight ahead.", [] {
            HeadTrackingRequestRecenter();
            if (Aer()) AerRequestRecenter("overlay");
            Message("view recentred");
        }));
        Item aim = Toggle("Aim where you look", "Lara, the crosshair and shots follow your head; the stick still turns the base direction.",
                          "aim.head_aim", [] { return HeadAimEnabled(); }, [](bool on) { SetHeadAimEnabled(on, "overlay"); });
        aim.enabled = [] { return Aer(); };
        p.items.push_back(aim);
        p.items.push_back(Toggle("Head turns the game camera", "In first person your head turns the game's own camera (rope arrows and climbing aim where you look).",
                                 "aim.head_drive", [] { return HeadDriveEnabled(); }, [](bool on) { if (HeadDriveEnabled() != on) HeadDriveToggle("overlay"); }));
        p.items.push_back(Number("Stick turn speed limit", "The right stick turns the first-person view at most this fast. Fast turns blur the image.",
                                 "firstperson.stick_turn_max_deg_s", 10.0f, [](float v) { return OffOr(v, "%.0f deg/s", "game's own"); }));
        p.items.push_back(Number("Look-down limit", "The stick can't take the view further below horizontal than this (your head can).",
                                 "firstperson.look_down_max_deg", 5.0f, [](float v) { return OffOr(v, "%.0f deg", "off"); }));
        p.items.push_back(Number("Body turns from", "Standing still, Lara turns towards where you look once it is this far from where she faces.",
                                 "firstperson.body_turn_deg", 5.0f, [](float v) { return OffOr(v, "%.0f deg", "off"); }));
        p.items.push_back(Number("Body turn speed", "How fast Lara turns to follow your view.", "firstperson.body_turn_speed", 10.0f,
                                 [](float v) { return Fmt("%.0f deg/s", v); }));
        p.items.push_back(Number("Hide body when looking back", "Lara's body isn't drawn while you look further than this from where she faces.",
                                 "firstperson.hide_body_deg", 5.0f, [](float v) { return OffOr(v, "beyond %.0f deg", "never"); }));
        p.items.push_back(Number("Neck model", "Looking down, the eye moves forward up to this far, so you see her body from in front.",
                                 "firstperson.neck_forward_cm", 1.0f, [](float v) { return OffOr(v, "%.0f cm", "off"); }));
        {
            Item eye;
            eye.kind = Kind::Number;
            eye.label = "Eye position forward";
            eye.help = "How far in front of Lara's head bone the first-person eye sits.";
            eye.value = [] { return Fmt("%.0f cm", HeadBoneEyeForwardCm()); };
            eye.change = [](int d) {
                const float v = HeadBoneEyeForwardCm() + d;
                if (v < -10.0f || v > 20.0f) return;
                HeadBoneAdjustEye(static_cast<float>(d), 0.0f, 0.0f);
                SaveExtra("firstperson.eye_forward_cm", Fmt("%g", HeadBoneEyeForwardCm()));
            };
            eye.reset = [] {
                HeadBoneAdjustEye(6.0f - HeadBoneEyeForwardCm(), 0.0f, 0.0f);
                SaveExtra("firstperson.eye_forward_cm", Fmt("%g", HeadBoneEyeForwardCm()));
            };
            eye.capture = [] {
                const float v = HeadBoneEyeForwardCm();
                return std::function<void()>([v] {
                    HeadBoneAdjustEye(v - HeadBoneEyeForwardCm(), 0.0f, 0.0f);
                    SaveExtra("firstperson.eye_forward_cm", Fmt("%g", v));
                });
            };
            p.items.push_back(eye);
        }
        {
            Item st = Toggle("Camera stabilisation", "Keeps the bob of Lara's walk and run animations out of the first-person view.", nullptr,
                             [] { return HeadBoneStabiliseOn(); }, [](bool on) {
                                 if (HeadBoneStabiliseOn() != on) HeadBoneToggleStabilise();
                                 SaveExtra("firstperson.stabilise", on ? "1" : "0");
                             });
            st.reset = nullptr;
            p.items.push_back(st);
        }
        p.items.push_back(Toggle("Hide Lara's head", "In first person her head, hair, bow and quiver aren't drawn (the bow shows while aiming).",
                                 "firstperson.hide_head", [] { return HideHeadEnabled(); }, [](bool on) { SetHideHeadEnabled(on, "overlay"); }));
        {
            // Shown as a percentage of life size (TrueWorldScale): lower world_scale = the world feels bigger. (An automatic
            // size from the player's eye height above the floor was tried and removed, 2026-10-27: the size that looked
            // right was the same standing and seated.)
            Item ws;
            ws.kind = Kind::Number;
            ws.label = "World size";
            ws.help = "How big the world looks: 100 % is life size, 200 % twice as big. Set what looks right to you.";
            ws.value = [] {   // the world looks bigger as the head / eye movement shrinks: size = true scale / world_scale
                return Fmt("%.0f %%", TrueWorldScale() / SettingGet("render.world_scale") * 100.0f);
            };
            auto apply = [](float v) {
                SettingSet("render.world_scale", v);
                AerSetWorldScale(SettingGet("render.world_scale"));
            };
            ws.change = [apply](int d) {
                const float t = TrueWorldScale();
                const float pct = std::round(t / SettingGet("render.world_scale") * 20.0f) * 5.0f + d * 5.0f;   // 5 % steps of size
                if (pct >= 25.0f && pct <= 400.0f) apply(t / (pct / 100.0f));
            };
            ws.reset = [apply] { apply(TrueWorldScale()); };
            ws.capture = [apply] {
                const float v = SettingGet("render.world_scale");
                return std::function<void()>([v, apply] { if (SettingGet("render.world_scale") != v) apply(v); });
            };
            ws.enabled = [] { return Aer(); };
            p.items.push_back(ws);
        }
        ShowFirst(p, { "World size" });
        g_pages.push_back(std::move(p));
    }

    // --- Rendering
    {
        Page p{ "Rendering" };
        {
            Item a = Action("Apply minimum-spec settings", "For GPUs well below an RTX 5090: 70 % render resolution (or lower if set lower), no overscan, frame warp on, "
                            "72 Hz. Lower the game's own quality settings too.", [] { ApplyPreset(true); });
            a.preset = true;
            p.items.push_back(a);
            Item b = Action("Apply standard settings", "The mod's standard settings: 100 % render resolution, overscan 1.08, frame warp on.",
                            [] { ApplyPreset(false); });
            b.preset = true;
            p.items.push_back(b);
        }
        p.items.push_back(Choice("Render mode", "Alternate eye renders each eye in turn with its own head pose (recommended). Side by side uses the game's own 3D.",
                                 "render.mode", { "side by side", "mono", "alternate eye (AER)" }, true));
        {
            Item r = Number("Render resolution", "Like SteamVR's render resolution: 100 % is SteamVR's 100 %. Higher is sharper if your "
                            "card has headroom (SteamVR's Auto picked 150 % on an RTX 5090). auto: chosen for your card.",
                            "render.resolution_pct", 10.0f, [](float v) { return ResolutionText(v); }, nullptr, true);
            r.change = [](int d) {
                float v = SettingGet("render.resolution_pct");
                v = v <= 0.0f ? (d > 0 ? 100.0f : 90.0f) : v + d * 10.0f;
                if (v < 50.0f) v = 50.0f;
                if (v > 200.0f) { v = 0.0f; }
                SettingSet("render.resolution_pct", v);
                SettingSet("render.resolution_auto", 0.0f);   // the player's choice from now on
            };
            r.reset = [] { SettingSet("render.resolution_pct", 0.0f); SettingSet("render.resolution_auto", 0.0f); };
            p.items.push_back(r);
        }
        {
            Item afw = Toggle("Frame warp (AFW)", "The eye not rendered this frame is warped from the one that was: smoother motion in alternate-eye mode.",
                              "render.afw", [] { return AfwEnabled(); }, [](bool on) { AfwSetEnabled(on); });
            afw.enabled = [] { return Aer(); };
            // Canted eye views (afw.cpp): the setting stays, the warp stands down -- say why.
            afw.value = [] { return std::string(AfwEnabled() ? "On" : "Off") + (AfwEnabled() && AfwCantedEyes() ? " (inactive: canted eye views)" : ""); };
            p.items.push_back(afw);
        }
        p.items.push_back(Choice("In-engine cutscenes", "Stereo keeps 3D in cutscenes; mono shows the same image to both eyes.",
                                 "render.aer_cutscenes", { "stereo", "mono" }));
        {
            Item r;
            r.kind = Kind::Choice;
            r.label = "Headset refresh rate";
            r.help = "A lower rate the game holds steadily looks better than a higher one it misses.";
            r.value = [] { return g_xr.refreshHz > 0 ? Fmt("%.0f Hz", g_xr.refreshHz) : std::string("-"); };
            r.enabled = [] { return g_xr.rates.size() > 1; };
            r.change = [](int d) {
                const std::vector<float>& rs = g_xr.rates;
                if (rs.size() < 2) return;
                int i = 0;
                for (int k = 0; k < static_cast<int>(rs.size()); ++k) if (std::fabs(rs[k] - g_xr.refreshHz) < 0.5f) i = k;
                i = std::clamp(i + d, 0, static_cast<int>(rs.size()) - 1);
                g_refreshRequest = rs[i];
                SettingSet("render.refresh_hz", rs[i]);
                SaveSoon();
            };
            r.reset = [] { SettingSet("render.refresh_hz", 0.0f); SaveSoon(); Message("refresh rate: the runtime's own from the next start"); };
            r.capture = [] {
                const float hz = g_xr.refreshHz, saved = SettingGet("render.refresh_hz");
                return std::function<void()>([hz, saved] {
                    if (hz > 0.0f && std::fabs(hz - g_xr.refreshHz) > 0.5f) g_refreshRequest = hz;
                    SettingSet("render.refresh_hz", saved);
                });
            };
            p.items.push_back(r);
        }
        p.items.push_back(Number("Overscan", "Renders a little beyond the headset's view so fast head turns don't show black edges. Costs performance.",
                                 "render.overscan", 0.02f, [](float v) { return Fmt("%.2f", v); }));
        p.items.push_back(Number("Head pose prediction", "How many frames ahead the head pose is predicted for the camera (alternate eye).",
                                 "render.aer_predict_frames", 1.0f, [](float v) { return Fmt("%.0f frames", v); }));
        {
            Item s;
            s.kind = Kind::Toggle;
            s.label = "Flat screen mode";
            s.help = "Shows the whole game image on a flat screen in front of you, for menus and the map.";
            s.on = [] { return g_xr.screenMode; };
            s.value = [] { return std::string(g_xr.screenMode ? "On" : "Off"); };
            s.activate = [] { g_screenToggle = true; };
            s.change = [](int) { g_screenToggle = true; };
            s.capture = [] {
                const bool v = g_xr.screenMode;
                return std::function<void()>([v] { if (g_xr.screenMode != v) g_screenToggle = true; });
            };
            p.items.push_back(s);
        }
        const bool cheeky = GetFileAttributesW(L"CheekyFoveatedDLSS\\CheekyFoveatedDLSSRuntime.dll") != INVALID_FILE_ATTRIBUTES;
        {
            // Foveated rendering (CheekyFoveatedDLSS, chosen in Setup): each mode has its own sharp region in Cheeky's ini,
            // which Cheeky reads when it starts -- every change applies after a restart, kept in step also on undo.
            Item f = Choice("Foveated rendering", "Off: all at full quality. Fixed: middle sharp, edges softer, saves little. "
                            "Eye tracked: sharp where you look, about 15 % less GPU time.",
                            "foveation.mode", { "off", "fixed (centre)", "eye tracked" }, true);
            f.enabled = [cheeky] { return cheeky; };
            if (!cheeky) f.value = [] { return std::string("not installed"); };
            const auto setMode = [](float mode) {
                SettingSet("foveation.mode", mode);
                WriteCheekyFoveation(true);
                AutoResolutionFollow();
                SaveSoon();
            };
            f.change = [setMode](int d) { setMode(static_cast<float>(((static_cast<int>(SettingGet("foveation.mode")) + d) % 3 + 3) % 3)); };
            f.activate = [setMode] { setMode(static_cast<float>((static_cast<int>(SettingGet("foveation.mode")) + 1) % 3)); };
            f.reset = [setMode] { setMode(SettingDefault("foveation.mode")); };
            f.capture = [setMode] {
                const float v = SettingGet("foveation.mode");
                return std::function<void()>([v, setMode] { if (SettingGet("foveation.mode") != v) setMode(v); });
            };
            p.items.push_back(f);
        }
        {
            // The sharp region's size (Cheeky's Width / Height): each mode starts at its own (gpu_tier.cpp FoveaSize), and
            // keeps its shape when resized. The automatic resolution's timings are for the modes' own sizes.
            Item z;
            z.kind = Kind::Number;
            z.label = "Foveated region size";
            z.help = "How much of the view stays at full quality (width x height of the image). Larger: a glance away stays "
                     "sharp, but it saves less GPU time. Y: the mode's own size.";
            z.restart = true;
            z.enabled = [cheeky] { return cheeky && SettingGet("foveation.mode") >= 1.0f; };
            z.value = [cheeky] {
                if (!cheeky) return std::string("not installed");
                if (SettingGet("foveation.mode") < 1.0f) return std::string("foveated rendering off");
                float w = 0.0f, h = 0.0f;
                CheekyFoveaSize(&w, &h);
                return Fmt("%.0f %%", w * 100.0f) + " x " + Fmt("%.0f %%", h * 100.0f);
            };
            z.change = [](int d) {
                float w = 0.0f, h = 0.0f, dw = 0.0f, dh = 0.0f;
                CheekyFoveaSize(&w, &h);
                FoveaSize(CurrentFoveation(), &dw, &dh);
                const float nw = std::clamp(std::round((w + 0.05f * d) * 20.0f) / 20.0f, 0.10f, 0.90f);
                WriteCheekyFoveaSize(nw, std::clamp(nw * dh / dw, 0.05f, 0.95f));
            };
            z.reset = [] {
                float w = 0.0f, h = 0.0f;
                FoveaSize(CurrentFoveation(), &w, &h);
                WriteCheekyFoveaSize(w, h);
            };
            z.capture = [] {
                float w = 0.0f, h = 0.0f;
                CheekyFoveaSize(&w, &h);
                return std::function<void()>([w, h] {
                    float a = 0.0f, b = 0.0f;
                    CheekyFoveaSize(&a, &b);
                    if (std::fabs(a - w) > 0.001f || std::fabs(b - h) > 0.001f) WriteCheekyFoveaSize(w, h);
                });
            };
            p.items.push_back(z);
        }
        {
            // DLSS 5 (Cheeky's DLSS-NR): eye tracked only, with NVIDIA's runtime beside the game (Setup copies one).
            const bool runtime = GetFileAttributesW(L"nvngx_dlssnr.dll") != INVALID_FILE_ATTRIBUTES;
            Item n = Toggle("DLSS 5 (experimental)", "NVIDIA's neural rendering, for a more lifelike image. It uses about as much GPU "
                            "time as eye tracking saves. With eye tracked foveated rendering only.", "foveation.dlss5", nullptr,
                            [](bool) { WriteCheekyFoveation(false); AutoResolutionFollow(); }, true);
            n.enabled = [cheeky, runtime] { return cheeky && runtime && SettingGet("foveation.mode") >= 2.0f; };
            n.value = [cheeky, runtime] {
                if (!cheeky) return std::string("not installed");
                if (!runtime) return std::string("nvngx_dlssnr.dll not found");
                if (SettingGet("foveation.mode") < 2.0f) return std::string("eye tracked only");
                return std::string(SettingGet("foveation.dlss5") != 0.0f ? "On" : "Off");
            };
            p.items.push_back(n);
        }
        p.items.push_back(Toggle("Eye tracking through SteamVR", "Reads eye tracking from SteamVR when OpenXR offers none (foveated rendering).",
                                 "gaze.openvr", nullptr, nullptr, true));
        {
            // Last on the page: on by default (and in both presets), off only to troubleshoot.
            Item d = Toggle("DLSS history per eye (troubleshooting)", "Each eye keeps its own DLSS history: needed for clean foveated "
                            "rendering. Turn off only to compare ghosting or smearing, or with very little video memory.",
                            "render.dlss_per_eye", [] { return DlssPerEyeState() == 1; }, [](bool on) { DlssPerEyeSet(on); });
            d.enabled = [] { return DlssPerEyeState() >= 0; };
            p.items.push_back(d);
        }
        ShowFirst(p, { "Apply minimum-spec settings", "Apply standard settings", "Render resolution", "Foveated rendering",
                       "Foveated region size", "DLSS 5 (experimental)", "Eye tracking through SteamVR" });
        g_pages.push_back(std::move(p));
    }

    // --- HUD
    {
        Page p{ "HUD" };
        auto hudGet = [](bool* on, float* sc, float* oy) { return UiProbeHudGet(on, sc, oy); };
        {
            Item t;
            t.kind = Kind::Toggle;
            t.label = "Game HUD smaller and centred";
            t.help = "Draws the game's HUD smaller and in the middle of your view, where you can see it.";
            t.on = [hudGet] { bool on; float s, o; hudGet(&on, &s, &o); return on; };
            t.value = [hudGet] {
                bool on; float s, o;
                if (!hudGet(&on, &s, &o)) return std::string("after a restart");
                return std::string(on ? "On" : "Off");
            };
            auto flip = [hudGet] {
                bool on; float s, o;
                const bool installed = hudGet(&on, &s, &o);
                SaveExtra("hud.enabled", on ? "0" : "1");
                if (!installed) { Message("the HUD hooks start with the game: restart to apply"); return; }
                UiProbeHudSet(!on, s, o);
            };
            t.activate = flip;
            t.change = [flip](int) { flip(); };
            t.capture = [hudGet] {
                bool on; float sc, o;
                const bool installed = hudGet(&on, &sc, &o);
                return std::function<void()>([installed, on, sc, o] {
                    if (installed) UiProbeHudSet(on, sc, o);
                    SaveExtra("hud.enabled", on ? "1" : "0");
                    SaveExtra("hud.scale", Fmt("%g", sc));
                    SaveExtra("hud.offset_y", Fmt("%g", o));
                });
            };
            p.items.push_back(t);
        }
        {
            Item n;
            n.kind = Kind::Number;
            n.label = "HUD size";
            n.help = "The game's HUD as a share of its full size.";
            n.value = [hudGet] { bool on; float s, o; hudGet(&on, &s, &o); return Fmt("%.0f %%", s * 100.0f); };
            n.change = [hudGet](int d) {
                bool on; float s, o;
                hudGet(&on, &s, &o);
                s = std::round((s + d * 0.05f) * 20.0f) / 20.0f;
                UiProbeHudSet(on, s, o);
                hudGet(&on, &s, &o);
                SaveExtra("hud.scale", Fmt("%g", s));
            };
            n.reset = [hudGet] { bool on; float s, o; hudGet(&on, &s, &o); UiProbeHudSet(on, 0.6f, o); SaveExtra("hud.scale", "0.6"); };
            p.items.push_back(n);
        }
        {
            Item n;
            n.kind = Kind::Number;
            n.label = "HUD height";
            n.help = "Moves the HUD up or down in your view.";
            n.value = [hudGet] { bool on; float s, o; hudGet(&on, &s, &o); return o == 0.0f ? std::string("centre") : Fmt(o > 0 ? "%.0f %% lower" : "%.0f %% higher", std::fabs(o) * 100.0f); };
            n.change = [hudGet](int d) {
                bool on; float s, o;
                hudGet(&on, &s, &o);
                o = std::round((o + d * 0.02f) * 50.0f) / 50.0f;
                UiProbeHudSet(on, s, o);
                hudGet(&on, &s, &o);
                SaveExtra("hud.offset_y", Fmt("%g", o));
            };
            n.reset = [hudGet] { bool on; float s, o; hudGet(&on, &s, &o); UiProbeHudSet(on, s, 0.0f); SaveExtra("hud.offset_y", "0"); };
            p.items.push_back(n);
        }
        p.items.push_back(Toggle("HUD in its own layer", "The HUD is added over each eye after the frame warp, so it doesn't wobble with the scene.",
                                 "hud.layer", [] { return HudLayerEnabled(); }, [](bool on) { HudLayerSetEnabled(on); }));
        p.items.push_back(Number("Overlay distance", "How far in front of you this panel opens.", "overlay.distance_m", 0.1f,
                                 [](float v) { return Fmt("%.1f m", v); }, [](float) { g_place = true; }));
        p.items.push_back(Number("Overlay size", "How wide this panel is.", "overlay.width_m", 0.05f,
                                 [](float v) { return Fmt("%.2f m", v); }));
        g_pages.push_back(std::move(p));
    }

    // --- Controls
    {
        Page p{ "Controls" };
        p.items.push_back(Info("This overlay", [] { return std::string("Menu + Guide (Xbox button)"); },
                               "Hold Guide, then press Menu. The game keeps running; it doesn't see the gamepad while the overlay is open."));
        p.items.push_back(Info("In the overlay", [] { return std::string("D-pad, A select, B close"); },
                               "D-pad or left stick: choose (up/down) and change (left/right). LB / RB: pages. Y: back to the default."));
        p.items.push_back(Info("First / third person", [] { return std::string("LB + click right stick"); }));
        p.items.push_back(Info("Aim where you look", [] { return std::string("LB + hold right stick 0.6 s"); }));
        p.items.push_back(Info("Camera stabilisation", [] { return std::string("LB + click left stick"); }));
        p.items.push_back(Info("Keyboard", [] { return std::string("Home first person, K head aim, J log mark"); }));
        p.items.push_back(Toggle("View change tones", "Tones when the view switches between first and third person, and when menus pause head tracking.",
                                 "sound.view_cues"));
        g_pages.push_back(std::move(p));
    }

    // --- Support
    {
        Page p{ "Support" };
        p.items.push_back(Action("Mark this moment in the log", "Puts a MARK line in vrmod.log, so a problem you just saw is easy to find.", [] {
            EventMarkNow("overlay");
            Message("marked in vrmod.log");
        }));
        p.items.push_back(Action("Save a bug report folder", "Copies vrmod.log, the two previous logs and your settings into a new folder in the game folder.",
                                 [] { Message(SaveBugReport()); }));
        {
            // CheekyFoveatedDLSS's own logs ([CheekyFoveatedDLSS] Logging in its ini; off as installed, read when it
            // starts). For reports about foveated rendering; the bug report folder copies them.
            Item log = Toggle("Foveated rendering log", "For reports about foveated rendering: CheekyFoveatedDLSS writes its own log. "
                              "Turn it off again afterwards -- the log keeps growing.", nullptr,
                              [] { return CheekyLoggingOn(); },
                              [](bool on) { IniWriteKeys(kCheekyIni, { { "CheekyFoveatedDLSS.Logging", on ? "1" : "0" } }); }, true);
            const bool cheeky = GetFileAttributesW(L"CheekyFoveatedDLSS\\CheekyFoveatedDLSSRuntime.dll") != INVALID_FILE_ATTRIBUTES;
            log.enabled = [cheeky] { return cheeky; };
            if (!cheeky) log.value = [] { return std::string("not installed"); };
            p.items.push_back(log);
        }
        p.items.push_back(Info("Version", [] { return ModVersion(); }));
        p.items.push_back(Info("Log file", [] { return std::string("vrmod.log in the game folder"); }));
        p.items.push_back(Info("Source and updates", [] { return std::string("github.com/jcooray/Shadow-of-the-Tomb-Raider-VR"); },
                               "Free and open source (GPL-3.0). Report problems there too, with a bug report folder."));
        p.items.push_back(Info("Support development", [] { return std::string("ko-fi.com/hadeanvr"); },
                               "If you enjoy the mod, you can buy me a coffee there. Thank you!"));
        p.items.push_back(Info("Uses", [] { return std::string("MinHook (BSD-2-Clause), OpenXR SDK (MIT)"); },
                               "Camera patterns from IGCS (BSD-2-Clause). Full notices: SOTTR-VR\\THIRD_PARTY_NOTICES.md."));
        g_pages.push_back(std::move(p));
    }
}

bool Selectable(const Item& it) {
    return it.kind != Kind::Info;
}

void FixSelection() {
    Page& p = g_pages[g_page];
    int& s = g_sel[g_page];
    const int n = static_cast<int>(p.items.size());
    if (n == 0) return;
    s = std::clamp(s, 0, n - 1);
    if (!Selectable(p.items[s])) {
        for (int i = 0; i < n; ++i) if (Selectable(p.items[i])) { s = i; break; }
    }
}

void MoveSelection(int d) {
    Page& p = g_pages[g_page];
    const int n = static_cast<int>(p.items.size());
    int& s = g_sel[g_page];
    for (int i = s + d; i >= 0 && i < n; i += d) {
        if (Selectable(p.items[i])) { s = i; return; }
    }
}

bool ItemEnabled(const Item& it) {
    return !it.enabled || it.enabled();
}

// ---------------------------------------------------------------------------------------------------------------
// Input

struct Repeat {
    bool was = false;
    ULONGLONG next = 0;
    bool Fire(bool down, ULONGLONG now) {   // true on the press and then every 90 ms after 400 ms held
        bool fire = false;
        if (down && !was) { fire = true; next = now + 400; }
        else if (down && now >= next) { fire = true; next = now + 90; }
        was = down;
        return fire;
    }
};

void TakeSnapshot() {
    Page& p = g_pages[g_page];
    g_snap = Snapshot{};
    g_snap.page = g_page;
    g_snap.settings = SettingsSnapshot();
    for (const Item& it : p.items) {
        g_snap.values.push_back(it.value ? it.value() : std::string());
        g_snap.restore.push_back(it.capture ? it.capture() : std::function<void()>());
        g_snap.touched.push_back(false);
    }
}

// The page's changes since its snapshot: "label: old -> new" for each restorable item whose shown value differs.
std::vector<std::string> PageChanges() {
    std::vector<std::string> out;
    if (g_snap.page != g_page) return out;
    const Page& p = g_pages[g_page];
    for (size_t i = 0; i < p.items.size() && i < g_snap.values.size(); ++i) {
        if (!g_snap.restore[i] || !g_snap.touched[i]) continue;
        const std::string now = p.items[i].value ? p.items[i].value() : std::string();
        if (now != g_snap.values[i]) out.push_back(p.items[i].label + ":  " + g_snap.values[i] + "  ->  " + now);
    }
    return out;
}

// For a Render resolution change: the headset software's own resolution setting multiplies with it.
std::string ResolutionRuntimeNote() {
    std::string rt = g_xr.runtime + " " + g_xr.system;   // Virtual Desktop through SteamVR names itself in the system
    std::transform(rt.begin(), rt.end(), rt.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (rt.find("virtualdesktop") != std::string::npos || rt.find("virtual desktop") != std::string::npos)
        return "Virtual Desktop's quality preset multiplies with this: Godlike renders far more than Ultra or High. "
               "On mid-range graphics cards use High or Ultra, or keep this lower.";
    if (rt.find("steamvr") != std::string::npos)
        return "SteamVR's own render resolution multiplies with this -- leave SteamVR at 100 % and set it here.";
    return "Your headset software's own resolution setting multiplies with this -- leave it at its default and set it here.";
}

// Whether an item needs a game restart: marked * on its label and in the footnote.
bool NeedsRestart(const Item& it) { return it.restart || static_cast<bool>(it.restartFor); }

// Whether one of the page's changes only applies after a restart: then keeping them closes the game.
bool PageNeedsRestart() {
    if (g_snap.page != g_page) return false;
    const Page& p = g_pages[g_page];
    for (size_t i = 0; i < p.items.size() && i < g_snap.values.size(); ++i) {
        if (!g_snap.restore[i] || !g_snap.touched[i]) continue;
        const std::string now = p.items[i].value ? p.items[i].value() : std::string();
        if (now == g_snap.values[i]) continue;
        if (p.items[i].restart || (p.items[i].restartFor && p.items[i].restartFor(g_snap.values[i], now))) return true;
    }
    return false;
}

// Closes the game the way its window's close button would (it shuts down normally; the ExitProcess hook tears the
// OpenXR session down first). If it's still running after 10 s, it is ended.
void ExitGameToWindows(bool force = true) {
    modlog::Line("OVERLAY: closing the game so restart-only changes apply at the next start.");
    struct Find { DWORD pid; HWND hwnd; } f{ GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* f = reinterpret_cast<Find*>(l);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        f->hwnd = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&f));
    if (f.hwnd) PostMessageW(f.hwnd, WM_CLOSE, 0, 0);
    if (!force) return;   // in a level the game asks to confirm its quit message: the player answers it
    std::thread([] {
        Sleep(10000);
        modlog::Line("OVERLAY: the game didn't close by itself; ending it.");
        ExitProcess(0);
    }).detach();
}

void Close(const char* how) {
    g_open = false;
    g_releaseHold = true;
    g_confirm = Confirm{};
    modlog::Line(std::string("OVERLAY: closed (") + how + ")");
}

// Leave the page (+1 / -1) or close (2), first asking to keep or undo the page's changes if there are any.
void Leave(int then) {
    if (!PageChanges().empty()) {
        g_confirm.active = true;
        g_confirm.then = then;
        return;
    }
    if (then == 2) { Close("B"); return; }
    g_page = (g_page + then + static_cast<int>(g_pages.size())) % static_cast<int>(g_pages.size());
    FixSelection();
    TakeSnapshot();
}

void Answer(bool keep) {
    const std::vector<std::string> changes = PageChanges();
    const bool exitGame = keep && PageNeedsRestart();
    if (keep) {
        SaveNow();
        std::string list;
        for (const std::string& c : changes) list += (list.empty() ? "" : "; ") + c;
        modlog::Line("OVERLAY: kept " + std::to_string(changes.size()) + " change(s) on the " + g_pages[g_page].name + " page: " + list);
    } else {
        for (size_t i = 0; i < g_snap.restore.size(); ++i) if (g_snap.restore[i] && g_snap.touched[i]) g_snap.restore[i]();
        SettingsRestore(g_snap.settings);
        SaveNow();   // writes the restored values over anything marked changed
        std::string list;
        for (const std::string& c : changes) list += (list.empty() ? "" : "; ") + c;
        modlog::Line("OVERLAY: undid " + std::to_string(changes.size()) + " change(s) on the " + g_pages[g_page].name + " page: " + list);
    }
    const int then = g_confirm.then;
    g_confirm = Confirm{};
    TakeSnapshot();
    if (exitGame) {
        Close("kept changes, exiting the game");
        ExitGameToWindows();
        return;
    }
    if (then == 2) Close(keep ? "kept changes" : "undid changes");
    else Leave(then);
}

// A or B (or Guide + Menu) on the notice. Shown on its own, the panel opens now, on the page it was given.
// A / Enter (ok) or B on the notice. Its OK action runs; alone, the panel then opens on its page or the overlay closes.
void DismissNotice(bool ok) {
    if (!ok && g_notice.onOk && !g_notice.canLater) return;   // only its OK dismisses it
    const Notice n = g_notice;
    g_notice = Notice{};
    if (ok && n.onOk) n.onOk();
    if (!n.alone) return;
    if (n.opensPanel) {
        FixSelection();
        TakeSnapshot();
    } else {
        g_open = false;
        g_releaseHold = true;
    }
    g_lastSig.clear();
}

// Closes the game for the restart that applies the render resolution: the game asks to confirm its quit message (in a
// level), and the player starts it again. (A helper that started it again by itself was dropped, 2026-10-28: a small
// program waiting for one process to end and then starting another drew 6/71 virus-scanner detections, Microsoft's
// among them.)
void QuitForRestart() {
    modlog::Line("OVERLAY: closing the game for the restart that applies the render resolution");
    ExitGameToWindows(false);
}

// A notice on its own that only A / Enter dismisses (B does nothing): the first run's two.
void ShowFirstRunNotice(const std::string& heading, const std::string& text, const std::string& okText, std::function<void()> onOk) {
    g_notice = Notice{};
    g_notice.active = true;
    g_notice.alone = true;
    g_notice.sections = { { heading, text } };
    g_notice.okText = okText;
    g_notice.onOk = onOk ? std::move(onOk) : [] {};
    g_open = true;
    g_place = true;
    g_lastSig.clear();
}

// First start, at the menu: the settings chosen for this PC.
void ShowAutoSettingsNotice(bool restartLater, float pct) {
    const std::string gpu = DetectedGpuName();
    std::string fov;
    switch (CurrentFoveation()) {
    case Foveation::Fixed: fov = " (with fixed foveated rendering)"; break;
    case Foveation::Eye: fov = " (with eye tracked foveated rendering)"; break;
    case Foveation::EyeDlss5: fov = " (with eye tracked foveated rendering and DLSS 5)"; break;
    default: break;
    }
    const std::string chosen = "Render resolution " + Fmt("%.0f", pct) + " % for " + (gpu.empty() ? std::string("your graphics card") : "your " + gpu)
                             + " and headset" + fov + ".";
    ShowFirstRunNotice("Auto settings detected",
                       chosen + (restartLater ? " It applies after a restart: you'll be asked to restart the game once a level has loaded."
                                              : "") + " Change any setting later in the overlay (Guide + Menu).",
                       "A / Enter: OK", nullptr);
    modlog::Line(std::string("OVERLAY: auto settings notice") + (restartLater ? " (the restart is asked for once a level has loaded)" : ""));
}

// First run, once a level has loaded and play has gone on for 3 s: restart, to apply the render resolution. The game asks
// to confirm its quit message (only in a level); then the player starts it again.
void FirstRestartPrompt(ULONGLONG now) {
    static ULONGLONG playSince = 0;
    if (!g_restartPrompt) return;
    if (g_open || now < g_toastUntil || !g_xr.eyeW || MenuScreenActive() || CinematicActive() || !IsHeadTrackingEnabled()) {
        playSince = 0;
        return;
    }
    if (!playSince) playSince = now;
    if (now - playSince < 3000) return;
    g_restartPrompt = false;
    ShowFirstRunNotice("Restart the game",
                       "The render resolution chosen for your PC applies after a restart. Press A, confirm the game's quit message, "
                       "then start the game again.",
                       "A / Enter: quit the game", [] { QuitForRestart(); });
    modlog::Line("OVERLAY: restart notice (A / Enter, then the game's quit message; the player starts it again)");
}

void HandleInput(const PadState& pad, ULONGLONG now) {
    static Repeat up, down, left, right;
    static uint16_t prev = 0;
    const uint16_t b = pad.buttons;
    const auto pressed = [&](uint16_t bit) { return (b & bit) && !(prev & bit); };
    // The left stick counts only in its main direction, and well pushed: a diagonal push while scrolling must not change
    // the value on the line (2026-10-17: an unseen change was caught by the confirmation).
    const bool horizontal = std::abs(pad.lx) > std::abs(pad.ly);
    const bool sUp = !horizontal && pad.ly > 24000, sDown = !horizontal && pad.ly < -24000;
    const bool sLeft = horizontal && pad.lx < -24000, sRight = horizontal && pad.lx > 24000;
    if (g_notice.active) {   // the notice: A / Enter for OK, B (DismissNotice decides whether B may)
        static bool enterWas = true;
        const bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        const bool enterPressed = enter && !enterWas;
        enterWas = enter;
        if (pressed(XINPUT_GAMEPAD_A) || enterPressed) DismissNotice(true);
        else if (pressed(XINPUT_GAMEPAD_B)) DismissNotice(false);
        prev = b;
        return;
    }
    if (g_confirm.active) {   // the dialog: A keep, B undo, X stay on the page
        if (pressed(XINPUT_GAMEPAD_A)) Answer(true);
        else if (pressed(XINPUT_GAMEPAD_B)) Answer(false);
        else if (pressed(XINPUT_GAMEPAD_X)) g_confirm = Confirm{};
        prev = b;
        return;
    }
    Page& p = g_pages[g_page];
    if (pressed(XINPUT_GAMEPAD_RIGHT_SHOULDER)) { Leave(1); prev = b; return; }
    if (pressed(XINPUT_GAMEPAD_LEFT_SHOULDER)) { Leave(-1); prev = b; return; }
    if (up.Fire((b & XINPUT_GAMEPAD_DPAD_UP) || sUp, now)) MoveSelection(-1);
    if (down.Fire((b & XINPUT_GAMEPAD_DPAD_DOWN) || sDown, now)) MoveSelection(1);
    const int n = static_cast<int>(p.items.size());
    const int s = g_sel[g_page];
    Item* it = (s >= 0 && s < n && Selectable(p.items[s])) ? &p.items[s] : nullptr;
    const bool usable = it && ItemEnabled(*it);
    const auto touch = [&] {
        if (g_snap.page != g_page || s >= static_cast<int>(g_snap.touched.size())) return;
        if (it->preset) std::fill(g_snap.touched.begin(), g_snap.touched.end(), true);
        else g_snap.touched[s] = true;
    };
    if (left.Fire((b & XINPUT_GAMEPAD_DPAD_LEFT) || sLeft, now) && usable && it->change && it->kind != Kind::Toggle) { it->change(-1); touch(); }
    if (right.Fire((b & XINPUT_GAMEPAD_DPAD_RIGHT) || sRight, now) && usable && it->change && it->kind != Kind::Toggle) { it->change(1); touch(); }
    if (usable && it->kind == Kind::Toggle && (pressed(XINPUT_GAMEPAD_DPAD_LEFT) || pressed(XINPUT_GAMEPAD_DPAD_RIGHT)) && it->change) { it->change(1); touch(); }
    if (pressed(XINPUT_GAMEPAD_A) && usable && it->activate) { it->activate(); touch(); }
    if (pressed(XINPUT_GAMEPAD_Y) && usable && it->reset) { it->reset(); touch(); Message("back to the default"); }
    if (pressed(XINPUT_GAMEPAD_B)) Leave(2);
    prev = b;
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing

void EnsureCanvas() {
    if (g_dc) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kW;
    bi.bmiHeader.biHeight = -kH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    g_dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    g_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_px = static_cast<uint32_t*>(bits);
    SelectObject(g_dc, g_bmp);
    SetBkMode(g_dc, TRANSPARENT);
    auto font = [](int h, int w) { return CreateFontW(-h, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI"); };
    g_fTitle = font(40, FW_SEMIBOLD);
    g_fTab = font(30, FW_SEMIBOLD);
    g_fRow = font(31, FW_NORMAL);
    g_fHelp = font(27, FW_NORMAL);
    g_fKeys = font(27, FW_BOLD);
}

void Fill(int x0, int y0, int x1, int y1, COLORREF c) {
    RECT r{ x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(g_dc, &r, b);
    DeleteObject(b);
}

// A raised button: a vertical gradient (top to bottom colour), rounded top corners, a light edge along the top and left
// and a dark one along the right and bottom -- the page tabs.
void Bevel(int x0, int y0, int x1, int y1, COLORREF top, COLORREF bottom, COLORREF light, COLORREF dark) {
    const int h = (std::max)(1, y1 - y0 - 1);
    auto mix = [h](COLORREF a, COLORREF b, int i) {
        auto ch = [&](int ca, int cb) { return ca + (cb - ca) * i / h; };
        return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
    };
    for (int y = y0; y < y1; ++y) {
        const int inset = y == y0 ? 3 : y == y0 + 1 ? 1 : 0;   // rounded top corners
        Fill(x0 + inset, y, x1 - inset, y + 1, mix(top, bottom, y - y0));
    }
    Fill(x0 + 3, y0, x1 - 3, y0 + 2, light);       // top edge
    Fill(x0, y0 + 3, x0 + 2, y1, light);           // left edge
    Fill(x1 - 2, y0 + 3, x1, y1, dark);            // right edge
    Fill(x0 + 2, y1 - 2, x1, y1, dark);            // bottom edge
}

// An image blended onto the canvas at (x, y) (GDI drawing flushed first: this writes the pixels directly).
void Blit(int x, int y, const uint32_t* img, int w, int h) {
    GdiFlush();
    for (int j = 0; j < h; ++j) {
        if (y + j < 0 || y + j >= kH) continue;
        for (int i = 0; i < w; ++i) {
            if (x + i < 0 || x + i >= kW) continue;
            const uint32_t s = img[j * w + i];
            const uint32_t a = s >> 24;
            if (!a) continue;
            uint32_t& d = g_px[(y + j) * kW + (x + i)];
            auto ch = [&](int shift) {
                return ((((s >> shift) & 0xFF) * a + ((d >> shift) & 0xFF) * (255 - a)) / 255) << shift;
            };
            d = ch(16) | ch(8) | ch(0);
        }
    }
}

void Text(int x0, int y0, int x1, int y1, HFONT f, COLORREF c, const std::string& s, UINT flags) {
    RECT r{ x0, y0, x1, y1 };
    SelectObject(g_dc, f);
    SetTextColor(g_dc, c);
    const std::wstring w = W(s);
    DrawTextW(g_dc, w.c_str(), -1, &r, flags | DT_NOPREFIX);
}

// Everything the panel shows, as one string: redrawn only when it changes.
std::string Signature() {
    std::string sig = std::to_string(g_page) + "|" + std::to_string(g_sel[g_page]) + "|" + (GetTickCount64() < g_msgUntil ? g_msg : "")
        + (g_confirm.active ? "|confirm" : "") + (g_notice.active ? "|notice" : "");
    for (const Item& it : g_pages[g_page].items) {
        sig += "|" + (it.value ? it.value() : std::string()) + (ItemEnabled(it) ? "" : "#");
    }
    return sig;
}

void FinishImage(bool panelShape);

// The panel closed, a message showing: a banner across the top of an otherwise transparent image.
void RenderToast() {
    EnsureCanvas();
    if (!g_px) return;
    Fill(0, 0, kW, kH, RGB(255, 0, 255));   // key colour: transparent
    // The box fits the wrapped text (DT_VCENTER doesn't centre more than one line).
    RECT m{ 120, 0, kW - 120, 0 };
    SelectObject(g_dc, g_fRow);
    const std::wstring wt = W(g_toast);
    DrawTextW(g_dc, wt.c_str(), -1, &m, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    const int textH = (std::min)(static_cast<int>(m.bottom), 400);
    Fill(80, 20, kW - 80, 20 + textH + 60, kAccent);
    Fill(84, 24, kW - 84, 16 + textH + 60, kBar);
    Text(120, 50, kW - 120, 50 + textH, g_fRow, kText, g_toast, DT_LEFT | DT_WORDBREAK);
    GdiFlush();
    FinishImage(false);
}

// The notice box, centred on the canvas: its sections, each a heading and text (with an image left of it, when given),
// divided by rules, and the OK line.
void DrawNotice() {
    // Each section: its heading, then its text wrapped to the box (measured first, so the box fits them).
    const int bw = 1000, bx = (kW - bw) / 2, tw = bw - 60;
    std::vector<int> textH, bodyH;
    int bh = 20 + 70;   // top margin, the OK line
    for (const auto& sec : g_notice.sections) {
        RECT m{ 0, 0, tw - (sec.image ? sec.imageW + 30 : 0), 0 };
        SelectObject(g_dc, g_fRow);
        const std::wstring w = W(sec.text);
        DrawTextW(g_dc, w.c_str(), -1, &m, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        textH.push_back(m.bottom);
        bodyH.push_back((std::max)(static_cast<int>(m.bottom), sec.image ? sec.imageH : 0));
        bh += 55 + bodyH.back() + 30;   // heading, text (or image), gap and rule
    }
    const int by = (std::max)(130, (kH - bh) / 2);
    Fill(bx - 4, by - 4, bx + bw + 4, by + bh + 4, kAccent);
    Fill(bx, by, bx + bw, by + bh, kBar);
    int y = by + 20;
    for (size_t i = 0; i < g_notice.sections.size(); ++i) {
        const NoticeSection& sec = g_notice.sections[i];
        if (i > 0) Fill(bx + 30, y - 16, bx + bw - 30, y - 14, kDim);   // the rule between sections
        Text(bx + 30, y, bx + bw - 30, y + 50, g_fTab, kText, sec.heading, DT_LEFT | DT_SINGLELINE);
        y += 55;
        int tx = bx + 30;
        if (sec.image) {
            Blit(tx, y + (bodyH[i] - sec.imageH) / 2, sec.image, sec.imageW, sec.imageH);
            tx += sec.imageW + 30;
        }
        Text(tx, y + (bodyH[i] - textH[i]) / 2, bx + bw - 30, y + (bodyH[i] + textH[i]) / 2, g_fRow, kText, sec.text, DT_LEFT | DT_WORDBREAK);
        y += bodyH[i] + 30;
    }
    Text(bx + 30, by + bh - 60, bx + bw - 30, by + bh - 20, g_fTab, kAccent, g_notice.okText, DT_LEFT | DT_SINGLELINE);
}

void Render() {
    EnsureCanvas();
    if (!g_px) return;
    if (g_notice.active && g_notice.alone) {   // the notice on its own: transparent around the box
        Fill(0, 0, kW, kH, RGB(255, 0, 255));
        DrawNotice();
        GdiFlush();
        FinishImage(false);
        return;
    }
    Fill(0, 0, kW, kH, kBg);
    // Title and page tabs.
    Fill(0, 0, kW, 120, kBar);
    Text(36, 14, 600, 64, g_fTitle, kText, "SOTTR VR", DT_LEFT | DT_SINGLELINE);
    Text(600, 22, kW - 36, 64, g_fKeys, kKeys, "LB / RB: pages    B: close", DT_RIGHT | DT_SINGLELINE);
    const int np = static_cast<int>(g_pages.size());
    const int tabW = (kW - 48) / np;
    // Tabs as raised buttons: the others a little lower and grey, the page's own taller, blue and joined to the page
    // below by its accent edge.
    for (int i = 0; i < np; ++i) {
        const int x = 24 + i * tabW;
        if (i == g_page) continue;
        Fill(x + 8, 118, x + tabW - 2, 120, RGB(14, 15, 18));   // its shadow on the bar
        Bevel(x + 4, 78, x + tabW - 4, 117, RGB(66, 71, 83), RGB(42, 45, 53), RGB(100, 107, 122), RGB(16, 18, 22));
        Text(x, 80, x + tabW, 116, g_fTab, RGB(170, 176, 186), g_pages[i].name, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    {
        const int x = 24 + g_page * tabW;
        Bevel(x + 2, 68, x + tabW - 2, 120, RGB(86, 132, 210), RGB(40, 66, 118), RGB(150, 188, 250), RGB(20, 30, 52));
        Fill(x + 2, 116, x + tabW - 2, 120, kAccent);
        Text(x, 70, x + tabW, 116, g_fTab, kText, g_pages[g_page].name, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    // Rows.
    Page& p = g_pages[g_page];
    const int n = static_cast<int>(p.items.size());
    // A page with restart-only items keeps its last row for the footnote explaining the *.
    const bool footnote = std::any_of(p.items.begin(), p.items.end(), NeedsRestart);
    const int visible = (kListBottom - kListTop) / kRowH - (footnote ? 1 : 0);
    if (footnote) Text(44, kListTop + visible * kRowH, kW - 44, kListTop + (visible + 1) * kRowH - 4, g_fHelp, kDim,
                       "* Changes to this require restarting the game", DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    int& scroll = g_scroll[g_page];
    const int sel = g_sel[g_page];
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + visible) scroll = sel - visible + 1;
    scroll = std::clamp(scroll, 0, (std::max)(0, n - visible));
    for (int r = 0; r < visible && scroll + r < n; ++r) {
        const Item& it = p.items[scroll + r];
        const int y = kListTop + r * kRowH;
        const bool selected = (scroll + r) == sel && Selectable(it);
        const bool en = ItemEnabled(it);
        if (selected) Fill(24, y, kW - 24, y + kRowH - 4, kSelBg);
        const COLORREF labelC = !en ? kDim : kText;
        std::string label = it.label + (NeedsRestart(it) ? " *" : "");
        Text(44, y, 760, y + kRowH - 4, g_fRow, labelC, label, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        std::string v = it.value ? it.value() : "";
        if (!en && it.kind != Kind::Info) v += it.kind == Kind::Action ? "" : "  (not available)";
        COLORREF vc = kText;
        if (it.kind == Kind::Info) vc = kDim;
        if (it.kind == Kind::Toggle && it.on && en) vc = it.on() ? kOn : kOff;
        if (!en) vc = kDim;
        if (selected && en && (it.kind == Kind::Number || it.kind == Kind::Choice)) v = "<  " + v + "  >";
        if (it.kind == Kind::Info) Text(330, y, kW - 44, y + kRowH - 4, g_fRow, vc, v, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        else Text(700, y, kW - 44, y + kRowH - 4, g_fRow, vc, v, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    // Help for the selected item, or a message.
    Fill(0, kListBottom + 20, kW, kH, kBar);
    std::string help;
    COLORREF hc = kDim;
    if (GetTickCount64() < g_msgUntil) { help = g_msg; hc = kWarn; }
    else if (sel >= 0 && sel < n) help = p.items[sel].help;
    if (p.items.size() && sel >= 0 && sel < n && Selectable(p.items[sel]) && GetTickCount64() >= g_msgUntil) {
        const Item& it = p.items[sel];
        std::string keys = it.kind == Kind::Action ? "A: do it" : it.kind == Kind::Toggle ? "A: switch" : "Left / right: change";
        if (it.reset) keys += "    Y: default";
        Text(36, kH - 46, kW - 36, kH - 10, g_fHelp, kAccent, keys, DT_LEFT | DT_SINGLELINE);
    }
    Text(36, kListBottom + 30, kW - 36, kH - 50, g_fHelp, hc, help, DT_LEFT | DT_WORDBREAK);
    if (g_confirm.active) {
        const std::vector<std::string> ch = PageChanges();
        const bool restart = PageNeedsRestart();
        const int rows = (std::min)(static_cast<int>(ch.size()), 7);
        // Notes under the changes (two lines each): what the headset software adds to a resolution change, and that
        // keeping a restart-only change closes the game.
        std::vector<std::pair<std::string, COLORREF>> notes;
        if (std::any_of(ch.begin(), ch.end(), [](const std::string& c) { return c.rfind("Render resolution", 0) == 0; }))
            notes.emplace_back(ResolutionRuntimeNote(), kText);
        if (restart) notes.emplace_back("These changes need the game restarted. Keeping them closes the game to Windows -- "
                                        "progress since the last checkpoint isn't saved. Then start it again.", kWarn);
        const int extra = 80 * static_cast<int>(notes.size());
        const int bw = 1100, bh = 210 + rows * 40 + extra, bx = (kW - bw) / 2, by = (kH - bh) / 2;
        Fill(bx - 4, by - 4, bx + bw + 4, by + bh + 4, kAccent);
        Fill(bx, by, bx + bw, by + bh, kBar);
        Text(bx + 30, by + 20, bx + bw - 30, by + 70, g_fTab, kText,
             std::string("Keep the changes on the ") + g_pages[g_page].name + " page?", DT_LEFT | DT_SINGLELINE);
        for (size_t i = 0; i < notes.size(); ++i) {
            const int top = by + bh - 60 - extra + static_cast<int>(i) * 80;
            Text(bx + 40, top, bx + bw - 30, top + 70, g_fHelp, notes[i].second, notes[i].first, DT_LEFT | DT_WORDBREAK);
        }
        for (int i = 0; i < rows; ++i) {
            Text(bx + 40, by + 80 + i * 40, bx + bw - 30, by + 120 + i * 40, g_fHelp, kText, ch[i], DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (static_cast<int>(ch.size()) > rows) Text(bx + 40, by + 80 + rows * 40, bx + bw - 30, by + 120 + rows * 40, g_fHelp, kDim,
                                                     "and " + std::to_string(ch.size() - rows) + " more", DT_LEFT | DT_SINGLELINE);
        Text(bx + 30, by + bh - 60, bx + bw - 30, by + bh - 20, g_fTab, kAccent,
             restart ? "A: keep and exit to Windows     B: undo     X: stay" : "A: keep     B: undo     X: stay on this page",
             DT_LEFT | DT_SINGLELINE);
    }
    if (g_notice.active) DrawNotice();
    if (scroll > 0 || scroll + visible < n) {   // more rows above / below the visible ones
        const std::string more = std::string("more ") + (scroll > 0 ? "\xE2\x96\xB2" : "") + (scroll + visible < n ? "\xE2\x96\xBC" : "");
        Text(kW - 300, kH - 46, kW - 36, kH - 10, g_fHelp, kDim, more, DT_RIGHT | DT_SINGLELINE);
    }
    GdiFlush();
    FinishImage(true);
}

// The bitmap as BGRA with alpha for the OpenXR side. Panel: opaque inside the rounded rectangle, transparent outside its
// corners. Otherwise (a message): the key colour is transparent.
void FinishImage(bool panelShape) {
    g_image.resize(static_cast<size_t>(kW) * kH * 4);
    const int rad = 28;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const uint32_t c = g_px[y * kW + x];
            if (!panelShape) {
                uint8_t* o = &g_image[(static_cast<size_t>(y) * kW + x) * 4];
                const bool key = (c & 0xFFFFFF) == 0xFF00FF;   // RGB(255, 0, 255) in the DIB's BGRx
                o[0] = static_cast<uint8_t>(c & 0xFF);
                o[1] = static_cast<uint8_t>((c >> 8) & 0xFF);
                o[2] = static_cast<uint8_t>((c >> 16) & 0xFF);
                o[3] = key ? 0 : 255;
                continue;
            }
            int dx = 0, dy = 0;
            if (x < rad) dx = rad - x; else if (x >= kW - rad) dx = x - (kW - rad - 1);
            if (y < rad) dy = rad - y; else if (y >= kH - rad) dy = y - (kH - rad - 1);
            float a = 1.0f;
            if (dx && dy) a = std::clamp(rad + 0.5f - std::sqrt(static_cast<float>(dx * dx + dy * dy)), 0.0f, 1.0f);
            uint8_t* o = &g_image[(static_cast<size_t>(y) * kW + x) * 4];
            o[0] = static_cast<uint8_t>(c & 0xFF);
            o[1] = static_cast<uint8_t>((c >> 8) & 0xFF);
            o[2] = static_cast<uint8_t>((c >> 16) & 0xFF);
            o[3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }
    g_imageNew = true;
}

}  // namespace

void VrOverlaySetXrInfo(const OverlayXrInfo& info) {
    g_xr = info;
}

// Once per install (overlay.world_asked): the first time play starts -- alternate eye, a level loaded, head tracked, no
// menu or cutscene for 3 s -- a notice on its own: change World size if the world looks too large or too small, first
// person to start with and the combo that switches to third person. Dismissed, the overlay opens on the View page at
// World size.
void FirstWorldSizePrompt(ULONGLONG now) {
    static bool done = false;
    static ULONGLONG playSince = 0;
    if (done) return;
    if (SettingGet("overlay.world_asked") != 0.0f) { done = true; return; }
    if (g_open || now < g_toastUntil || !g_resChecked || g_restartPending || !g_xr.eyeW || !Aer() || MenuScreenActive() || CinematicActive()
        || !IsHeadTrackingEnabled()) {
        playSince = 0;
        return;
    }
    if (!playSince) playSince = now;
    if (now - playSince < 3000) return;
    done = true;
    SettingSet("overlay.world_asked", 1.0f);
    SettingsWriteKeys({ { "overlay.world_asked", "1" } });
    for (int i = 0; i < static_cast<int>(g_pages.size()); ++i) {
        if (std::string(g_pages[i].name) != "View") continue;
        g_page = i;
        g_sel[i] = 0;   // World size, first on the page
    }
    g_open = true;
    g_place = true;
    g_lastSig.clear();
    g_notice.active = true;
    g_notice.alone = true;
    g_notice.opensPanel = true;
    g_notice.sections = {
        { "World size", "If the world looks too large or too small, change World size, the first line of the overlay that opens "
                        "next, with left / right. 100 % is life size. Open the overlay any time with Guide + Menu." },
        { "First and third person", "You start in first person. Switch between first and third person any time with LB + "
                                    "click the right stick (keyboard: Home).", kViewToggleImage, kViewToggleImageW, kViewToggleImageH },
    };
    modlog::Line("OVERLAY: first-play notice (World size, first / third person); the overlay opens at World size after it");
}

void VrOverlayTick() {
    OverlayPadPoll();
    const ULONGLONG now = GetTickCount64();
    {
        // Frame rate over the last second (one tick per presented frame).
        static LARGE_INTEGER f{}, last{};
        static double winStart = 0.0, sumMs = 0.0, maxMs = 0.0;
        static int frames = 0;
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        const double ms = static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        if (last.QuadPart) {
            const double dt = ms - static_cast<double>(last.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
            sumMs += dt;
            maxMs = (std::max)(maxMs, dt);
            ++frames;
        }
        last = t;
        if (winStart == 0.0) winStart = ms;
        if (ms - winStart >= 1000.0 && frames) {
            g_fps = frames * 1000.0 / (ms - winStart);
            g_frameMsAvg = sumMs / frames;
            g_frameMsMax = maxMs;
            winStart = ms;
            sumMs = maxMs = 0.0;
            frames = 0;
        }
    }
    if (g_pages.empty()) BuildPages();

    PadState pad;
    OverlayPadRead(&pad);
    static bool comboWas = false, keyWas = false;
    const bool combo = (pad.buttons & kPadGuide) && (pad.buttons & XINPUT_GAMEPAD_START);
    // Keyboard fallback (Ctrl+Alt+O) for when the Guide button doesn't reach the game (e.g. Steam or the Xbox Game Bar
    // take it): open with the keyboard, use the gamepad inside.
    const bool key = (GetAsyncKeyState('O') & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    if ((combo && !comboWas) || (key && !keyWas)) {
        if (!g_open) {
            g_open = true;
            g_place = true;
            FixSelection();
            TakeSnapshot();
            g_lastSig.clear();
            modlog::Line(std::string("OVERLAY: opened (") + (combo ? "Menu + Guide" : "Ctrl+Alt+O") + ")");
        } else if (g_notice.active) {
            DismissNotice(false);
        } else if (!g_confirm.active) {
            Leave(2);   // closes, or asks first if the page has changes
        }
        g_releaseHold = true;
    } else if (g_open && !combo) {
        HandleInput(pad, now);
    }
    comboWas = combo;
    keyWas = key;
    // The game is blind to the pad while the overlay is open, and until every button pressed in it is released.
    if (g_releaseHold && pad.buttons == 0 && pad.lt < 30 && pad.rt < 30) g_releaseHold = false;
    OverlayPadBlockGame(g_open || g_releaseHold);
    StartupMessages();
    FirstRestartPrompt(now);
    FirstWorldSizePrompt(now);

    if (!g_open) {
        if (now < g_toastUntil) {
            const std::string sig = "toast|" + g_toast;
            if (sig != g_lastSig) { g_lastSig = sig; RenderToast(); }
        }
        return;
    }
    // Redraw when anything shown changed (the Status page's numbers at most twice a second).
    static ULONGLONG lastStatus = 0;
    if (g_page == 0 && now - lastStatus < 500 && !g_lastSig.empty()) return;
    lastStatus = now;
    const std::string sig = Signature();
    if (sig == g_lastSig) return;
    g_lastSig = sig;
    Render();
}

bool VrOverlayVisible() {
    return g_open || GetTickCount64() < g_toastUntil;
}

void VrOverlayNotify(const std::string& text, int seconds) {
    if (g_open) {   // the panel is open: in its help line
        Message(text);
        return;
    }
    g_toast = text;
    g_toastUntil = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    g_place = true;
    g_lastSig.clear();
}

bool VrOverlayTakePlace() {
    const bool p = g_place;
    g_place = false;
    return p;
}

const uint8_t* VrOverlayTakeImage(int* w, int* h) {
    if (!g_imageNew) return nullptr;
    g_imageNew = false;
    *w = kW;
    *h = kH;
    return g_image.data();
}

float VrOverlayWidthM() {
    return SettingGet("overlay.width_m");
}

float VrOverlayDistanceM() {
    return SettingGet("overlay.distance_m");
}

float VrOverlayTakeRefreshRequest() {
    const float r = g_refreshRequest;
    g_refreshRequest = 0.0f;
    return r;
}

bool VrOverlayTakeScreenModeToggle() {
    const bool t = g_screenToggle;
    g_screenToggle = false;
    return t;
}

}  // namespace vrmod
