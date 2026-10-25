#include "settings.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "logger.h"

namespace vrmod {

namespace {

constexpr const char* kFile = "vrmod_settings.ini";

std::map<std::string, std::string> ReadFile() {
    std::map<std::string, std::string> kv;
    std::ifstream in(kFile);
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t e = line.find(']');
            if (e != std::string::npos) section = line.substr(1, e - 1);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[section + "." + line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

float GetFloat(const std::map<std::string, std::string>& kv, const std::string& key, float fallback) {
    auto it = kv.find(key);
    if (it == kv.end()) return fallback;
    try { return std::stof(it->second); } catch (...) { return fallback; }
}

int GetInt(const std::map<std::string, std::string>& kv, const std::string& key, int fallback) {
    auto it = kv.find(key);
    if (it == kv.end()) return fallback;
    try { return std::stoi(it->second); } catch (...) { return fallback; }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Live settings (in-headset overlay, vr_overlay.cpp). Each key below is read from vrmod_settings.ini once, then held
// as a live value: the getters return it every call, so the overlay can change it while the game runs. Out-of-range
// values in the file fall back to the default (clamp = false) or are clamped (clamp = true), as the getters always did.
// Choice keys (render.mode, render.aer_cutscenes) hold the index into their names.
namespace {

struct LiveDef {
    const char* key;
    float def, lo, hi;
    bool clamp;
    const char* names;   // choice keys: '|'-separated names, saved by name; nullptr = a number
};

const LiveDef kLive[] = {
    // New-install defaults (2026-10-18): alternate-eye rendering, 100 % render resolution, overscan 1.08 (the standard
    // preset), the world at life size (world_scale 1 / 1.8, vr_overlay.cpp TrueWorldScale -- World size 100 %).
    { "render.mode", 2, 0, 2, false, "sbs|mono|aer" },
    { "render.world_scale", 1.0f / 1.8f, 0.05f, 20.0f, false, nullptr },
    { "render.overscan", 1.08f, 1.0f, 1.5f, false, nullptr },
    { "render.refresh_hz", 0.0f, 0.0f, 240.0f, false, nullptr },
    { "render.dlss_per_eye", 1, 0, 1, false, nullptr },
    { "render.aer_pair_follow_d2c", 1, 0, 1, false, nullptr },
    { "render.aer_cutscenes", 0, 0, 1, false, "stereo|mono" },
    { "render.aer_predict_frames", 3, 0, 4, false, nullptr },
    { "render.afw", 1, 0, 1, false, nullptr },
    { "firstperson.hide_gear", 0, 0, 1, false, nullptr },
    { "firstperson.look_limit_deg", 0, 0, 180, true, nullptr },
    { "firstperson.neck_forward_cm", 14, 0, 40, true, nullptr },
    { "firstperson.hide_body_down_deg", 0, 0, 89, true, nullptr },
    { "firstperson.look_down_max_deg", 70, 0, 89, true, nullptr },
    { "firstperson.stick_turn_max_deg_s", 120, 0, 1000, true, nullptr },
    { "firstperson.body_turn_deg", 35, 0, 170, true, nullptr },
    { "firstperson.body_turn_speed", 200, 30, 1000, true, nullptr },
    { "firstperson.hide_body_deg", 90, 0, 180, true, nullptr },
    { "firstperson.near_clip", 0, 0, 1, false, nullptr },
    { "firstperson.hide_head", 1, 0, 1, false, nullptr },
    { "gaze.cheeky_openxr_layer", 0, 0, 1, false, nullptr },
    { "gaze.openvr", 1, 0, 1, false, nullptr },
    { "hud.layer", 1, 0, 1, false, nullptr },
    { "sound.view_cues", 0, 0, 1, false, nullptr },
    { "aim.head_drive", 1, 0, 1, false, nullptr },
    { "aim.head_aim", 1, 0, 1, false, nullptr },
    { "render.resolution_pct", 100, 0, 200, false, nullptr },   // 0 = as set in the game / vrmod_fakedesktop.txt
    // 1 = resolution_pct is chosen by the mod for the graphics card and the headset target (a new install, until the
    // overlay changes it); re-chosen when the target changes (another headset, the runtime's quality setting).
    { "render.resolution_auto", 0, 0, 1, false, nullptr },
    // 1 = the automatic choice expects this graphics card to miss 72 Hz even at its lowest (50 %): the headset says so
    // once, when the game runs at that resolution (vr_overlay.cpp), and clears it.
    { "render.gpu_note", 0, 0, 1, false, nullptr },
    // The graphics card's speed against the RTX 5090 (gpu_tier.cpp), stored with the automatic choice: the overlay chooses
    // again with it when foveated rendering changes.
    { "render.gpu_relative", 0, 0, 4, false, nullptr },
    // The foveated rendering the automatic choice was made for (gpu_tier.h Foveation: 0 off, 1 fixed, 2 eye, 3 eye + DLSS 5).
    { "render.auto_foveation", 0, 0, 3, false, nullptr },
    { "render.target_w", 0, 0, 16384, false, nullptr },       // the headset's target render size (PHASE6), for resolution_pct
    { "render.target_h", 0, 0, 16384, false, nullptr },
    // Foveated rendering with CheekyFoveatedDLSS (optional, set by the installer): off = Cheeky disabled (its ini, after a
    // restart), fixed = a large sharp region in the middle, eye = a small one that follows the gaze when the headset
    // supplies one (else it stays in the middle). The region's size is in Cheeky's ini (gpu_tier.cpp FoveaSize).
    // cheeky_host.cpp / vr_overlay.cpp.
    { "foveation.mode", 2, 0, 2, false, "off|fixed|eye" },
    // DLSS 5 (Cheeky's DLSS-NR, NrEnabled in its ini): wanted by the player (installer / overlay); runs only eye tracked,
    // with nvngx_dlssnr.dll beside the game (Setup copies one from another game on the PC; never shipped with the mod).
    { "foveation.dlss5", 0, 0, 1, false, nullptr },
    { "overlay.distance_m", 1.0f, 0.5f, 3.0f, true, nullptr },
    { "overlay.width_m", 0.9f, 0.4f, 2.0f, true, nullptr },
    // 1 = the overlay has opened once at World size, with its notice (vr_overlay.cpp FirstWorldSizePrompt).
    { "overlay.world_asked", 0, 0, 1, false, nullptr },
};
constexpr int kLiveCount = static_cast<int>(sizeof(kLive) / sizeof(kLive[0]));

std::atomic<float> g_live[kLiveCount];
std::atomic<bool> g_liveLoaded{ false };
std::mutex g_liveMutex;
bool g_liveDirty[kLiveCount] = {};

std::string Trim(std::string v) {
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    return v;
}

int ChoiceIndex(const char* names, const std::string& v) {
    int i = 0;
    for (const char* p = names; *p; ++i) {
        const char* e = std::strchr(p, '|');
        const size_t n = e ? static_cast<size_t>(e - p) : std::strlen(p);
        if (v.size() == n && v.compare(0, n, p, n) == 0) return i;
        if (!e) break;
        p = e + 1;
    }
    return -1;
}

std::string ChoiceName(const char* names, int index) {
    int i = 0;
    for (const char* p = names; *p; ++i) {
        const char* e = std::strchr(p, '|');
        const size_t n = e ? static_cast<size_t>(e - p) : std::strlen(p);
        if (i == index) return std::string(p, n);
        if (!e) break;
        p = e + 1;
    }
    return "";
}

void LoadLive() {
    if (g_liveLoaded.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_liveMutex);
    if (g_liveLoaded.load(std::memory_order_relaxed)) return;
    const auto kv = ReadFile();
    for (int i = 0; i < kLiveCount; ++i) {
        const LiveDef& d = kLive[i];
        float v = d.def;
        auto it = kv.find(d.key);
        if (it != kv.end()) {
            if (d.names) {
                const int c = ChoiceIndex(d.names, Trim(it->second));
                if (c >= 0) v = static_cast<float>(c);
                else modlog::Line(std::string("Settings: ") + d.key + "=" + Trim(it->second) + " is not known (" + d.names + "); using "
                                  + ChoiceName(d.names, static_cast<int>(d.def)) + ".");
            } else {
                try { v = std::stof(it->second); } catch (...) { v = d.def; }
                if (v < d.lo || v > d.hi) v = d.clamp ? (v < d.lo ? d.lo : d.hi) : d.def;
            }
        }
        g_live[i].store(v, std::memory_order_relaxed);
    }
    g_liveLoaded.store(true, std::memory_order_release);
}

int LiveIndex(const char* key) {
    for (int i = 0; i < kLiveCount; ++i) if (std::strcmp(kLive[i].key, key) == 0) return i;
    return -1;
}

// The live value of a table key (loads the file on first use).
float Live(const char* key) {
    LoadLive();
    static thread_local const char* lastKey = nullptr;
    static thread_local int lastIndex = -1;
    const int i = (key == lastKey) ? lastIndex : LiveIndex(key);
    lastKey = key;
    lastIndex = i;
    return i >= 0 ? g_live[i].load(std::memory_order_relaxed) : 0.0f;
}

} // namespace

float SettingGet(const char* key) {
    LoadLive();
    const int i = LiveIndex(key);
    return i >= 0 ? g_live[i].load(std::memory_order_relaxed) : 0.0f;
}

bool SettingSet(const char* key, float value) {
    LoadLive();
    const int i = LiveIndex(key);
    if (i < 0) return false;
    const LiveDef& d = kLive[i];
    if (value < d.lo) value = d.lo;
    if (value > d.hi) value = d.hi;
    if (d.names) value = std::floor(value + 0.5f);
    g_live[i].store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_liveMutex);
    g_liveDirty[i] = true;
    return true;
}

std::vector<float> SettingsSnapshot() {
    LoadLive();
    std::vector<float> v(kLiveCount);
    for (int i = 0; i < kLiveCount; ++i) v[i] = g_live[i].load(std::memory_order_relaxed);
    return v;
}

void SettingsRestore(const std::vector<float>& snapshot) {
    if (static_cast<int>(snapshot.size()) != kLiveCount) return;
    for (int i = 0; i < kLiveCount; ++i) {
        // Measured by the mod, not chosen: the headset's target size and the true world scale stay as they are now.
        const std::string k = kLive[i].key;
        if (k == "render.target_w" || k == "render.target_h") continue;
        if (g_live[i].load(std::memory_order_relaxed) != snapshot[i]) SettingSet(kLive[i].key, snapshot[i]);
    }
}

float SettingDefault(const char* key) {
    const int i = LiveIndex(key);
    return i >= 0 ? kLive[i].def : 0.0f;
}

namespace {

std::string FormatLive(int i) {
    const LiveDef& d = kLive[i];
    const float v = g_live[i].load(std::memory_order_relaxed);
    if (d.names) return ChoiceName(d.names, static_cast<int>(v));
    std::ostringstream o;
    o << v;
    return o.str();
}

} // namespace

int SettingsSaveChanged() {
    std::vector<std::pair<std::string, std::string>> changed;   // "section.key" -> value
    {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        for (int i = 0; i < kLiveCount; ++i) {
            if (!g_liveDirty[i]) continue;
            g_liveDirty[i] = false;
            changed.emplace_back(kLive[i].key, FormatLive(i));
        }
    }
    if (changed.empty()) return 0;
    return SettingsWriteKeys(changed) ? static_cast<int>(changed.size()) : 0;
}

bool SettingsWriteKeys(const std::vector<std::pair<std::string, std::string>>& keys) {
    return IniWriteKeys(kFile, keys);
}

bool IniWriteKeys(const char* file, const std::vector<std::pair<std::string, std::string>>& keys) {
    // In place: an existing "key=" line in its section gets the new value; a missing key is added at the end of its
    // section, a missing section at the end of the file. Comments, order and unknown keys stay as they are.
    const char* const kFile = file;   // (shadows the settings file name: the same code for any ini)
    std::vector<std::string> lines;
    {
        std::ifstream in(kFile);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    for (const auto& kv : keys) {
        const size_t dot = kv.first.find('.');
        if (dot == std::string::npos) continue;
        const std::string section = kv.first.substr(0, dot), name = kv.first.substr(dot + 1);
        std::string cur;
        int sectionEnd = -1, found = -1;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            const std::string& l = lines[i];
            if (!l.empty() && l[0] == '[') {
                const size_t e = l.find(']');
                cur = e == std::string::npos ? "" : l.substr(1, e - 1);
                continue;
            }
            if (cur != section) continue;
            if (!Trim(l).empty()) sectionEnd = i;
            const size_t eq = l.find('=');
            if (eq != std::string::npos && l[0] != ';' && l[0] != '#' && Trim(l.substr(0, eq)) == name) found = i;
        }
        const std::string line = name + "=" + kv.second;
        if (found >= 0) {
            lines[found] = line;
        } else if (sectionEnd >= 0) {
            lines.insert(lines.begin() + sectionEnd + 1, line);
        } else {
            bool hasSection = false;
            for (const std::string& l : lines) if (l == "[" + section + "]") hasSection = true;
            if (hasSection) {   // an empty section: right after its header
                for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
                    if (lines[i] == "[" + section + "]") { lines.insert(lines.begin() + i + 1, line); break; }
                }
            } else {
                if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
                lines.push_back("[" + section + "]");
                lines.push_back(line);
            }
        }
    }
    const std::string tmp = std::string(kFile) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out) {
            modlog::Line("Settings: could not write vrmod_settings.ini.");
            return false;
        }
        for (const std::string& l : lines) out << l << "\r\n";
    }
    if (!MoveFileExA(tmp.c_str(), kFile, MOVEFILE_REPLACE_EXISTING)) {
        modlog::Line(std::string("Settings: could not replace ") + kFile + " (error " + std::to_string(GetLastError()) + ").");
        return false;
    }
    std::string list;
    for (const auto& kv : keys) list += (list.empty() ? "" : ", ") + kv.first + "=" + kv.second;
    modlog::Line(std::string("Settings: saved ") + (std::strcmp(kFile, "vrmod_settings.ini") ? std::string(kFile) + ": " : std::string()) + list);
    return true;
}

const LogSwitches& GetLogSwitches() {
    static const LogSwitches switches = [] {
        LogSwitches s;
        auto kv = ReadFile();
        s.campos = GetInt(kv, "logging.campos", s.campos ? 1 : 0) != 0;
        s.perf = GetInt(kv, "logging.perf", s.perf ? 1 : 0) != 0;
        s.events = GetInt(kv, "logging.events", s.events ? 1 : 0) != 0;
        return s;
    }();
    return switches;
}

const DebugSwitches& GetDebugSwitches() {
    static const DebugSwitches switches = [] {
        DebugSwitches s;
        auto kv = ReadFile();
        s.frameCsv = GetInt(kv, "debug.frame_csv", s.frameCsv ? 1 : 0) != 0;
        s.d3d12DebugLayer = GetInt(kv, "debug.d3d12_debug_layer", s.d3d12DebugLayer ? 1 : 0) != 0;
        s.uiProbe = GetInt(kv, "debug.ui_probe", 0) != 0;
        return s;
    }();
    return switches;
}

RenderMode GetRenderMode() {
    // The startup value for the whole session: the pipeline is built for one mode. The overlay's change is saved and
    // applies at the next start.
    static const int m = static_cast<int>(Live("render.mode"));
    return m == 1 ? RenderMode::Mono : m == 2 ? RenderMode::Aer : RenderMode::Sbs;
}

const char* RenderModeName(RenderMode mode) {
    return mode == RenderMode::Mono ? "mono" : mode == RenderMode::Aer ? "aer" : "sbs";
}

namespace {
float g_worldScaleForSave = -1.0f;
}

float GetRenderWorldScale() {
    return Live("render.world_scale");
}

void SetRenderWorldScaleForSave(float scale) {
    g_worldScaleForSave = scale;
}

float GetRenderOverscan() {
    return Live("render.overscan");
}

namespace {
float g_refreshHzForSave = -1.0f;
}

float GetRefreshHz() {
    return Live("render.refresh_hz");
}

void SetRefreshHzForSave(float hz) {
    g_refreshHzForSave = hz;
}

bool GetDlssPerEye() {
    return Live("render.dlss_per_eye") != 0.0f;
}

bool GetHideGearSetting() {
    return Live("firstperson.hide_gear") != 0.0f;
}

float GetLookLimitDegSetting() {
    return Live("firstperson.look_limit_deg");
}

float GetNeckForwardCmSetting() {
    return Live("firstperson.neck_forward_cm");
}

float GetHideBodyDownDegSetting() {
    return Live("firstperson.hide_body_down_deg");
}

float GetLookDownMaxDegSetting() {
    return Live("firstperson.look_down_max_deg");
}

float GetStickTurnMaxDegSetting() {
    return Live("firstperson.stick_turn_max_deg_s");
}

float GetBodyTurnDegSetting() {
    return Live("firstperson.body_turn_deg");
}

float GetBodyTurnSpeedSetting() {
    return Live("firstperson.body_turn_speed");
}

float GetHideBodyDegSetting() {
    return Live("firstperson.hide_body_deg");
}

bool GetCheekyOpenXrLayerAllowed() {
    return Live("gaze.cheeky_openxr_layer") != 0.0f;
}

bool GetGazeOpenVr() {
    return Live("gaze.openvr") != 0.0f;
}

bool GetAerPairFollowD2c() {
    return Live("render.aer_pair_follow_d2c") != 0.0f;
}

bool GetAerCutscenesMono() {
    return Live("render.aer_cutscenes") != 0.0f;
}

namespace {
HeadCamSettings g_headForSave;
bool g_haveHeadForSave = false;
}

HeadCamSettings& HeadCamSettingsLive() {
    static HeadCamSettings h = [] {
        auto kv = ReadFile();
        HeadCamSettings s;
        s.enabled = GetInt(kv, "firstperson.head_camera", 1) != 0;
        const int bone = GetInt(kv, "firstperson.head_bone", s.bone);
        if (bone >= 0 && bone < 1024) s.bone = bone;
        auto clampF = [](float v, float lo, float hi, float def) { return (v >= lo && v <= hi) ? v : def; };
        s.eyeFwdCm = clampF(GetFloat(kv, "firstperson.eye_forward_cm", s.eyeFwdCm), -50.0f, 50.0f, s.eyeFwdCm);
        s.eyeUpCm = clampF(GetFloat(kv, "firstperson.eye_up_cm", s.eyeUpCm), -50.0f, 50.0f, s.eyeUpCm);
        s.eyeSideCm = clampF(GetFloat(kv, "firstperson.eye_side_cm", s.eyeSideCm), -50.0f, 50.0f, s.eyeSideCm);
        s.smoothMs = clampF(GetFloat(kv, "firstperson.head_smooth_ms", s.smoothMs), 0.0f, 1000.0f, s.smoothMs);
        s.nearCm = clampF(GetFloat(kv, "firstperson.near_cm", s.nearCm), 1.0f, 60.0f, s.nearCm);
        s.nearRunCm = clampF(GetFloat(kv, "firstperson.near_run_cm", s.nearRunCm), 1.0f, 60.0f, s.nearRunCm);
        s.nearRunFromMps = clampF(GetFloat(kv, "firstperson.near_run_from_mps", s.nearRunFromMps), 0.0f, 20.0f, s.nearRunFromMps);
        s.nearRunFullMps = clampF(GetFloat(kv, "firstperson.near_run_full_mps", s.nearRunFullMps), 0.0f, 20.0f, s.nearRunFullMps);
        if (s.nearRunFullMps <= s.nearRunFromMps) s.nearRunFullMps = s.nearRunFromMps + 0.1f;
        s.stabilise = GetInt(kv, "firstperson.stabilise", 1) != 0;
        s.stabVerticalCm = clampF(GetFloat(kv, "firstperson.stab_vertical_cm", s.stabVerticalCm), 0.0f, 40.0f, s.stabVerticalCm);
        s.stabHorizontalCm = clampF(GetFloat(kv, "firstperson.stab_horizontal_cm", s.stabHorizontalCm), 0.0f, 40.0f, s.stabHorizontalCm);
        s.stabSmoothMs = clampF(GetFloat(kv, "firstperson.stab_smooth_ms", s.stabSmoothMs), 0.0f, 1000.0f, s.stabSmoothMs);
        return s;
    }();
    return h;
}

const HeadCamSettings& GetHeadCamSettings() {
    return HeadCamSettingsLive();
}

void SetHeadCamForSave(const HeadCamSettings& s) {
    g_headForSave = s;
    g_haveHeadForSave = true;
}

namespace {
HudSettings g_hudForSave;
bool g_haveHudForSave = false;
}

const HudSettings& GetHudSettings() {
    static const HudSettings h = [] {
        auto kv = ReadFile();
        HudSettings s;
        s.enabled = GetInt(kv, "hud.enabled", 1) != 0;
        const float sc = GetFloat(kv, "hud.scale", s.scale);
        if (sc >= 0.2f && sc <= 1.0f) s.scale = sc;
        const float oy = GetFloat(kv, "hud.offset_y", s.offsetY);
        if (oy >= -0.4f && oy <= 0.4f) s.offsetY = oy;
        return s;
    }();
    return h;
}

void SetHudForSave(const HudSettings& s) {
    g_hudForSave = s;
    g_haveHudForSave = true;
}

namespace {
int g_headAimForSave = -1;   // -1 = not changed live
}

bool GetHudLayerSetting() {
    return Live("hud.layer") != 0.0f;
}

bool GetViewCuesSetting() {
    return Live("sound.view_cues") != 0.0f;
}

bool GetAfwSetting() {
    return Live("render.afw") != 0.0f;
}

bool GetNearClipSetting() {
    return Live("firstperson.near_clip") != 0.0f;
}

bool GetHideHeadSetting() {
    return Live("firstperson.hide_head") != 0.0f;
}

bool GetHeadDriveSetting() {
    return Live("aim.head_drive") != 0.0f;
}

bool GetHeadAimSetting() {
    return Live("aim.head_aim") != 0.0f;
}

void SetHeadAimForSave(bool on) {
    g_headAimForSave = on ? 1 : 0;
}

int GetAerPredictFrames() {
    return static_cast<int>(Live("render.aer_predict_frames"));
}

bool SettingsFirstRun() {
    static const bool first = [] {
        // First run: no settings file -- or only what the installer wrote ([foveation] mode), no [render] section yet.
        if (GetFileAttributesA(kFile) != INVALID_FILE_ATTRIBUTES) {
            std::ifstream in(kFile);
            std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (all.find("[render]") != std::string::npos) return false;
        }
        LoadLive();   // the installer's keys, before the file is rewritten
        SettingSet("render.resolution_auto", 1.0f);   // chosen for the graphics card once the headset target is known
        SaveSettings(LoadSettings());   // every section, with comments, at the defaults
        // ...and the keys SaveSettings doesn't write.
        std::vector<std::pair<std::string, std::string>> extra;
        for (const char* k : { "render.afw", "render.resolution_pct", "render.resolution_auto", "firstperson.hide_head",
                               "firstperson.near_clip", "foveation.mode", "foveation.dlss5",
                               "overlay.distance_m", "overlay.width_m" }) {
            const int i = LiveIndex(k);   // choice keys (foveation.mode) by name: a number isn't read back
            if (i >= 0) extra.emplace_back(k, FormatLive(i));
        }
        SettingsWriteKeys(extra);
        modlog::Line("Settings: first run -- wrote vrmod_settings.ini with the defaults (alternate eye, 100 % render resolution).");
        return true;
    }();
    return first;
}

Settings LoadSettings() {
    Settings s;
    auto kv = ReadFile();
    if (kv.empty()) {
        return s;
    }
    s.firstPerson = GetInt(kv, "startup.first_person", s.firstPerson ? 1 : 0) != 0;
    s.distanceMetres = GetFloat(kv, "startup.distance_m", s.distanceMetres);
    s.heightMetres = GetFloat(kv, "startup.height_m", s.heightMetres);
    s.sideMetres = GetFloat(kv, "startup.side_m", s.sideMetres);
    s.latencyFrames = GetInt(kv, "startup.latency_frames", s.latencyFrames);
    s.ordinalMatching = GetInt(kv, "startup.ordinal_matching", s.ordinalMatching ? 1 : 0) != 0;
    s.gameFov = GetFloat(kv, "startup.game_fov", s.gameFov);
    for (int i = 0; i < 4; ++i) {
        const std::string sec = "profile" + std::to_string(i + 1) + ".";
        if (kv.count(sec + "distance_m")) {
            s.profiles[i].valid = true;
            s.profiles[i].distanceMetres = GetFloat(kv, sec + "distance_m", 0.0f);
            s.profiles[i].heightMetres = GetFloat(kv, sec + "height_m", 0.0f);
            s.profiles[i].sideMetres = GetFloat(kv, sec + "side_m", 0.0f);
        }
    }
    return s;
}

void SaveSettings(const Settings& s) {
    std::ofstream out(kFile, std::ios::trunc);
    if (!out) {
        modlog::Line("Settings: could not write vrmod_settings.ini.");
        return;
    }
    const LogSwitches& ls = GetLogSwitches();
    out << "; SOTTR VR mod settings. Change them in the headset (hold Guide and press Menu on the gamepad), or edit this file\n"
           "; with the game closed. A missing key takes its default.\n";
    out << "[logging]\ncampos=" << (ls.campos ? 1 : 0) << "\nperf=" << (ls.perf ? 1 : 0) << "\nevents=" << (ls.events ? 1 : 0) << "\n\n";
    const DebugSwitches& ds = GetDebugSwitches();
    out << "[debug]\nframe_csv=" << (ds.frameCsv ? 1 : 0) << "\nd3d12_debug_layer=" << (ds.d3d12DebugLayer ? 1 : 0) << "\nui_probe=" << (ds.uiProbe ? 1 : 0) << "\n\n";
    out << "[render]\n; sbs = game Stereoscopic 3D on (default) | mono = Stereoscopic 3D off, same frame to both eyes\n"
           "; aer = Stereoscopic 3D off, alternate-eye rendering\nmode="
        << RenderModeName(GetRenderMode()) << "\n; world_scale: head/eye movement multiplier for aer (1.0 = real size)\nworld_scale="
        << (g_worldScaleForSave > 0.0f ? g_worldScaleForSave : GetRenderWorldScale())
        << "\n; overscan: widen the FOV beyond the headset's view (1.0 = exact; e.g. 1.08 leaves room for reprojection)\noverscan="
        << GetRenderOverscan() << "\n; aer_predict_frames: head pose predicted this many frames ahead for AER camera writes (default 3)\naer_predict_frames="
        << GetAerPredictFrames()
        << "\n; refresh_hz: headset refresh rate to request (0 = the runtime's; Ctrl+B cycles the supported rates)\nrefresh_hz="
        << (g_refreshHzForSave >= 0.0f ? g_refreshHzForSave : GetRefreshHz())
        << "\n; aer_cutscenes: stereo = alternate-eye in in-engine cutscenes | mono = same image to both eyes\naer_cutscenes="
        << (GetAerCutscenesMono() ? "mono" : "stereo")
        << "\n; dlss_per_eye: with DLSS on, each eye keeps its own DLSS history (a second DLSS feature; Ctrl+Alt+D toggles)\ndlss_per_eye="
        << (GetDlssPerEye() ? 1 : 0)
        << "\n; aer_pair_follow_d2c: 1 = when DLSS's frame check finds the images one frame off, move the eye pairing to match (default)\naer_pair_follow_d2c="
        << (GetAerPairFollowD2c() ? 1 : 0) << "\n\n";
    {
        const HeadCamSettings& h = g_haveHeadForSave ? g_headForSave : GetHeadCamSettings();
        out << "[firstperson]\n; head_camera: 1 = first-person camera at Lara's head bone (Ctrl+Alt+Home toggles), 0 = the distance/height/side offsets\n"
            << "head_camera=" << (h.enabled ? 1 : 0) << "\nhead_bone=" << h.bone
            << "\n; eye offset from the head bone, cm (PageUp/PageDown forward, Alt+PageUp/PageDown up, Ctrl+PageUp/PageDown side)\n"
            << "eye_forward_cm=" << h.eyeFwdCm << "\neye_up_cm=" << h.eyeUpCm << "\neye_side_cm=" << h.eyeSideCm
            << "\n; head_smooth_ms: smoothing of the head's motion relative to the skeleton root (bob)\nhead_smooth_ms=" << h.smoothMs
            << "\n; near_cm: near clip distance in first person standing still, hides the head (Ctrl+Alt+[ / ] while still; the game's own is 5)\nnear_cm=" << h.nearCm
            << "\n; near_run_cm: near clip when running, hides the gear bobbing on her back (Ctrl+Alt+[ / ] while running); blended in between\n"
               "; near_run_from_mps and near_run_full_mps of her speed\nnear_run_cm=" << h.nearRunCm << "\nnear_run_from_mps=" << h.nearRunFromMps
            << "\nnear_run_full_mps=" << h.nearRunFullMps
            << "\n; stabilise: 1 = ignore the head's animation bob -- it moves the camera only beyond stab_vertical_cm up/down or"
               " stab_horizontal_cm sideways/fore-aft, then smoothed over stab_smooth_ms (gamepad LB + left stick click / Ctrl+Alt+S toggle)\n"
            << "stabilise=" << (h.stabilise ? 1 : 0) << "\nstab_vertical_cm=" << h.stabVerticalCm << "\nstab_horizontal_cm=" << h.stabHorizontalCm
            << "\nstab_smooth_ms=" << h.stabSmoothMs
            << "\n; hide_gear: 1 = hide the gear attached to Lara (bow, quiver, axe, rifles) in first person, shown while aiming"
               " (Ctrl+Alt+G cycles all / one at a time / off)\nhide_gear=" << (GetHideGearSetting() ? 1 : 0)
            << "\n; look_limit_deg: the right stick can't turn the view further than this from where Lara's body faces (0 = off)\nlook_limit_deg="
            << GetLookLimitDegSetting()
            << "\n; hide_body_deg: Lara's body isn't drawn while the view points further than this from where her body faces (0 = never)\nhide_body_deg="
            << GetHideBodyDegSetting()
            << "\n; body_turn_deg: standing still, her body turns towards the view once it is this far from where she faces (0 = off)\nbody_turn_deg="
            << GetBodyTurnDegSetting() << "\n; body_turn_speed: how fast she turns, deg/s\nbody_turn_speed=" << GetBodyTurnSpeedSetting()
            << "\n; stick_turn_max_deg_s: the right stick turns the first-person view at most this fast (0 = the game's rate)\nstick_turn_max_deg_s="
            << GetStickTurnMaxDegSetting()
            << "\n; look_down_max_deg: the stick can't take the view further below horizontal than this (head included; 0 = off)\nlook_down_max_deg="
            << GetLookDownMaxDegSetting()
            << "\n; hide_body_down_deg: her body isn't drawn while the view points further below horizontal than this (0 = never)\nhide_body_down_deg="
            << GetHideBodyDownDegSetting()
            << "\n; neck_forward_cm: looking down, the eye moves forward up to this many cm (seeing her body from in front; 0 = off)\nneck_forward_cm="
            << GetNeckForwardCmSetting() << "\n\n";
    }
    {
        const HudSettings& h = g_haveHudForSave ? g_hudForSave : GetHudSettings();
        out << "[hud]\n; the game's UI drawn smaller and centred (Ctrl+Alt+8/9 smaller/larger, Ctrl+Alt+5/6 up/down, Ctrl+Alt+7 on/off)\n"
            << "enabled=" << (h.enabled ? 1 : 0) << "\nscale=" << h.scale << "\noffset_y=" << h.offsetY
            << "\n; layer: 1 = the HUD in its own layer, added over each eye after the AFW warp (Ctrl+Alt+L cycles)\nlayer="
            << (GetHudLayerSetting() ? 1 : 0) << "\n\n";
    }
    out << "[sound]\n; view_cues: 1 = tones when the view switches first/third person and when menus turn head tracking off/on\nview_cues="
        << (GetViewCuesSetting() ? 1 : 0) << "\n\n";
    out << "[gaze]\n; openvr: eye tracking from SteamVR's OpenVR interface (for foveated DLSS) when OpenXR gives none\nopenvr="
        << (GetGazeOpenVr() ? 1 : 0)
        << "\n; cheeky_openxr_layer: 1 = let Cheeky's system-wide OpenXR layer load into this game (0: this mod hosts Cheeky's gaze and eye views)\ncheeky_openxr_layer="
        << (GetCheekyOpenXrLayerAllowed() ? 1 : 0) << "\n\n";
    out << "[aim]\n; head_aim: aim where you look -- Lara, the crosshair and shots follow the head (render mode aer; K toggles)\nhead_aim="
        << (g_headAimForSave >= 0 ? g_headAimForSave : (GetHeadAimSetting() ? 1 : 0))
        << "\n; head_drive: in first person the head turns the game camera itself (Ctrl+Alt+E toggles)\nhead_drive=" << (GetHeadDriveSetting() ? 1 : 0) << "\n\n";
    out << "[startup]\n";
    out << "first_person=" << (s.firstPerson ? 1 : 0) << "\n";
    out << "distance_m=" << s.distanceMetres << "\n";
    out << "height_m=" << s.heightMetres << "\n";
    out << "side_m=" << s.sideMetres << "\n";
    out << "latency_frames=" << s.latencyFrames << "\n";
    out << "ordinal_matching=" << (s.ordinalMatching ? 1 : 0) << "\n";
    out << "game_fov=" << s.gameFov << "\n";
    for (int i = 0; i < 4; ++i) {
        if (!s.profiles[i].valid) continue;
        out << "\n[profile" << (i + 1) << "]\n";
        out << "distance_m=" << s.profiles[i].distanceMetres << "\n";
        out << "height_m=" << s.profiles[i].heightMetres << "\n";
        out << "side_m=" << s.profiles[i].sideMetres << "\n";
    }
    out.close();
    // Every live setting too (the overlay's: render resolution, frame warp, foveation, ...), which the sections above
    // don't carry: a whole-file save -- the development build's camera-profile keys, Ctrl+Alt+0..4 -- dropped them all
    // and the next start fell back to their defaults (2026-10-27: 100 % instead of 150 %).
    LoadLive();
    std::vector<std::pair<std::string, std::string>> live;
    for (int i = 0; i < kLiveCount; ++i) live.emplace_back(kLive[i].key, FormatLive(i));
    SettingsWriteKeys(live);
    modlog::Line("Settings: wrote vrmod_settings.ini.");
}

} // namespace vrmod
