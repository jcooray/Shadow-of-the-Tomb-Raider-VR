#pragma once

#include <string>
#include <utility>
#include <vector>

namespace vrmod {

// Persistent user settings, stored in vrmod_settings.ini next to the exe.
// The defaults below are the values the user settled on in play
// (first-person camera 3.1 m forward, height 0, head-tracking latency 3).
struct CameraProfile {
    bool valid = false;
    float distanceMetres = 0.0f;
    float heightMetres = 0.0f;
    float sideMetres = 0.0f;   // + = right
};

struct Settings {
    bool firstPerson = true;
    float distanceMetres = 3.1f;
    float heightMetres = 0.0f;
    float sideMetres = 0.0f;
    int latencyFrames = 3;
    bool ordinalMatching = true;   // frame-number head-pose matching (Ctrl+M toggles)
    float gameFov = 1.6f;          // vertical FOV (radians) forced on the game while head tracking is on
    CameraProfile profiles[4];   // hotkeys 1..4
};

// Optional log lines that can be switched off in vrmod_settings.ini ([logging] campos / perf / events).
struct LogSwitches {
    bool campos = false;   // once-a-second camera position + speed
    bool perf = true;      // heartbeat and XRPERF timing lines
    bool events = true;    // EVENT / ThirdPerson lines from the game's event bus
};

// Read once from vrmod_settings.ini on first use; cheap to call from anywhere.
const LogSwitches& GetLogSwitches();

// Development switches ([debug] in vrmod_settings.ini). Read once at startup.
struct DebugSwitches {
    bool frameCsv = false;          // start the per-frame CSV recorder at launch (Ctrl+J toggles it anyway)
    bool d3d12DebugLayer = false;   // enable the D3D12 debug layer before the game creates its device
    bool uiProbe = false;           // Brief 2 H0: D3D12 command-list probe for the HUD (Ctrl+Alt+U), docs/HUD.md
};
const DebugSwitches& GetDebugSwitches();

// How the game's frame reaches the headset ([render] mode in vrmod_settings.ini). Read once at startup.
//   sbs  : the game renders side-by-side (Stereoscopic 3D on); left half -> left eye, right half -> right eye.
//   mono : the game renders one full frame (Stereoscopic 3D off); the same frame goes to both eyes, flat.
//          The base for alternate-eye rendering (the brief's `aer`, later phases).
//   aer  : alternate-eye rendering (Phase 5+): Stereoscopic 3D off; each game frame renders one eye with that eye's
//          own headset pose; each presented frame goes to its eye, the other eye keeps its last image.
enum class RenderMode { Sbs, Mono, Aer };
RenderMode GetRenderMode();
const char* RenderModeName(RenderMode mode);

// [render] world_scale: multiplies head/eye movement (1.0 = real size; game units are cm). Read once at startup;
// the live value (Ctrl+[ / Ctrl+]) is passed back for Ctrl+Alt+0 saves.
float GetRenderWorldScale();
void SetRenderWorldScaleForSave(float scale);

// [render] overscan (Phase 6): the symmetric FOV that covers the headset's view is widened by this factor (on the
// tangent of each half-angle) so rotational reprojection has image to show at the edges. 1.0 = exact cover.
float GetRenderOverscan();

// [render] aer_predict_frames (Phase 7): the head pose used for a camera write is predicted for the display time of the
// frame it will appear in: the current frame's predicted display time + this many display periods. Default 3: with 2
// (the Phase 4 estimate) the Phase 7 run measured the shown pose ~1 period older than its display (pred_err_ms: 82 %
// of frames +1 period, 17 % 0). 0 = the current frame's display time (the Phase 5/6 behaviour).
int GetAerPredictFrames();

// [render] refresh_hz (Phase 8): headset refresh rate to request through XR_FB_display_refresh_rate (0 = leave the
// runtime's). AER needs the game to hold the refresh rate steadily; a lower rate held beats a higher one missed.
// Ctrl+B cycles the supported rates live; the live value is passed back for Ctrl+Alt+0 saves.
float GetRefreshHz();
void SetRefreshHzForSave(float hz);

// [render] aer_cutscenes (Phase 8): stereo (default) keeps alternate-eye rendering in in-engine cutscenes while the game
// honours camera writes; mono renders them from the head centre and shows the same image to both eyes.
bool GetAerCutscenesMono();
// [render] dlss_per_eye (docs/DLSS.md D1): under AER, a second DLSS feature so each eye keeps its own history. Default 1
// (2026-10-27): with the same-eye motion vectors (D2) it is as clean as the single history, and CheekyFoveatedDLSS needs
// it (the single history smears under Cheeky, and Cheeky learns which image is which eye only from the two features).
// It costs a second set of DLSS buffers (video memory), no GPU time. (Before D2 the single history was the clean one.)
bool GetDlssPerEye();
// [render] aer_pair_follow_d2c: when DLSS's frame check (D2c) finds the evaluated frame off the prediction, move the AER
// eye pairing to match (default 1) instead of only re-routing DLSS -- an off-by-one pairing shows each eye the other
// eye's image. 0 = the old behaviour.
bool GetAerPairFollowD2c();
// [gaze] openvr (docs/DLSS.md, foveated DLSS): read eye tracking through SteamVR's OpenVR interface
// (IVRSystem::GetEyeTrackedFoveationCenter) when our OpenXR session's eye gaze action gives nothing. Default 1.
bool GetGazeOpenVr();
// [gaze] cheeky_openxr_layer: let CheekyFoveatedDLSS's system-wide OpenXR layer load into this game. Default 0 -- this
// mod supplies Cheeky's gaze and per-eye views, and the layer's presence makes the Cheeky fork ignore them.
bool GetCheekyOpenXrLayerAllowed();
// [firstperson] hide_gear (docs/FIRSTPERSON.md, gear hiding): hide the objects attached to Lara (bow, quiver, axe,
// rifles on her back) in first person while she isn't aiming. Default 0 (2026-09-30: Lara's child objects hide but the
// visible back gear is part of her own model); Ctrl+Alt+G cycles the modes live.
bool GetHideGearSetting();
// [firstperson] look_limit_deg (head_drive.cpp, look limit): in first person the right stick can't turn the view more
// than this far from where Lara's body faces (a human neck; 2026-10-10: turning 180 deg looked back through her own
// body). The real head is never limited, and the view is never dragged when she turns. Default 0 (off): her facing
// shifts with idle and turning animations, so the stop point moved around (user, 2026-10-10).
float GetLookLimitDegSetting();
// [firstperson] hide_body_deg (head_drive.cpp): in first person Lara's body/outfit model isn't drawn while the view (real
// head included) points more than this far from where her body faces -- looking back never shows her body from inside.
// Shown again 10 deg inside it; never hidden while aiming. Default 0 (off since the body turn; it also depends on the
// outfit model's draw signature, which changed). 0 = never.
float GetHideBodyDegSetting();
// [firstperson] body_turn_deg / body_turn_speed (body_turn.cpp): standing still, Lara's body turns towards the view
// the stick turned once it is more than body_turn_deg from where she faces (default 35; 0 = off), at up to
// body_turn_speed deg/s (default 200).
// [firstperson] stick_turn_max_deg_s (head_drive.cpp): in first person the right stick turns the view at most this
// fast (default 120; 0 = the game's own rate, ~170 deg/s). 2026-10-11: full-rate stick turns blurred the image
// (alternate-eye rendering, DLSS history); the body turn wasn't the cause.
float GetStickTurnMaxDegSetting();
// [firstperson] look_down_max_deg (head_drive.cpp): the right stick can't take the first-person view (stick and real
// head together) further below horizontal than this; the real head is never limited. Default 70; 0 = off
// (2026-10-12: looking down at the game's ~90 deg limit showed the inside of her body).
float GetLookDownMaxDegSetting();
// [firstperson] hide_body_down_deg (head_drive.cpp): her body isn't drawn either while the view (stick and real head)
// points more than this below horizontal -- the camera in her head looked down through her neck and chest from inside.
// Shown again 10 deg above it; never while aiming. Default 50; 0 = never.
float GetHideBodyDownDegSetting();
// [firstperson] neck_forward_cm (camera_interceptor.cpp, neck model): looking down, the first-person eye moves forward
// along the view, from 20 deg below horizontal up to this many cm at 70 deg, so her body is seen from in front instead
// of through her neck. Default 14; 0 = off.
float GetNeckForwardCmSetting();
float GetBodyTurnDegSetting();
float GetBodyTurnSpeedSetting();

// [firstperson] head-bone camera (docs/FIRSTPERSON.md): the first-person camera sits at Lara's head bone plus an eye
// offset (cm, along the game camera's horizontal forward / right, and world up), with the head's motion relative to
// the skeleton root smoothed (time constant in ms). Read at startup; the live values are passed back for Ctrl+Alt+0.
struct HeadCamSettings {
    bool enabled = true;
    int bone = 12;
    float eyeFwdCm = 6.0f;   // at her eyes, in front of the head bone (2026-10-13: at the bone it could sit in her neck); 0 = the bone itself
    float eyeUpCm = 0.0f;
    float eyeSideCm = 0.0f;
    float smoothMs = 100.0f;
    // Near clip distance while in first person (cm; the game's own is 5). Hides the head around the camera: 30 cm took
    // the head and shoulders (2026-09-24), the face reaches ~10-12 cm in front of the head bone.
    float nearCm = 12.0f;
    // Speed-dependent near clip (2026-09-30, user): the gear on her back bobs into view when she runs, so the near clip
    // is nearCm standing still and nearRunCm when running, blended between nearRunFromMps and nearRunFullMps of her
    // horizontal speed (skeleton root, lightly smoothed -- she accelerates almost instantly).
    float nearRunCm = 48.0f;          // user, 2026-09-30: she leans well forward when running
    float nearRunFromMps = 1.5f;      // walking ~1.4 m/s, running ~3.0, sprint peaks ~4.5 (HEADCAM log)
    float nearRunFullMps = 2.8f;
    // Stabilisation (docs/FIRSTPERSON.md): the head's offset from the root passes a dead-zone follower before the
    // smoothing -- it moves only when the head leaves a band of stabVerticalCm (up/down) / stabHorizontalCm (sideways
    // and fore-aft) around the camera, so stride bounce and sway stay out while crouching, climbing and leaning come
    // through. The followed offset is then smoothed over stabSmoothMs.
    bool stabilise = true;
    float stabVerticalCm = 6.0f;
    float stabHorizontalCm = 6.0f;
    float stabSmoothMs = 150.0f;
};
const HeadCamSettings& GetHeadCamSettings();
void SetHeadCamForSave(const HeadCamSettings& s);

// [hud] (Brief 2 H1, docs/HUD.md): the game's UI layer is drawn smaller and centred (scale of its full size; offset_y
// moves it down by that fraction of the height, negative = up). Menus and the map are left alone. Live: Ctrl+Alt+8/9
// smaller/larger, Ctrl+Alt+5/6 up/down, Ctrl+Alt+7 on/off; Ctrl+Alt+0 saves.
struct HudSettings {
    bool enabled = true;
    float scale = 0.6f;
    float offsetY = 0.0f;
};
const HudSettings& GetHudSettings();
void SetHudForSave(const HudSettings& s);

// [aim] (docs/AIM.md): head_aim = aim where you look (render mode aer): Lara, the crosshair and shots follow the head;
// the stick still turns the base direction. K toggles it live; Ctrl+Alt+0 saves. Default on.
bool GetHeadAimSetting();
// [aim] head_drive: in first person the head turns the game camera itself (docs/AIM.md, the real fix); Ctrl+Alt+E toggles.
bool GetHeadDriveSetting();
// [firstperson] hide_head: in first person, Lara's head model (face, eyes, mouth, hair) isn't drawn (docs/FIRSTPERSON.md);
// Ctrl+Alt+F5 toggles.
bool GetHideHeadSetting();
// [firstperson] near_clip: 1 = override the renderer's near plane in first person with near_cm / near_run_cm (the old
// way of hiding the head and gear); 0 (default) = the game's own.
bool GetNearClipSetting();
// [render] afw: the eye not rendered this frame is warped from the one that was (docs/AFW.md); Ctrl+Alt+I toggles.
bool GetAfwSetting();
// [hud] layer: the game's HUD drawn into a layer of our own and added over each eye after the AFW warp (docs/HUD.md,
// "HUD layer"), so it isn't warped with the scene depth under it; default 1.
bool GetHudLayerSetting();
// [sound] view_cues: tones when the view switches between first and third person (by hand or automatically) and when
// a menu / the map / loading turns head tracking off and back on; default 0 (silent, 2026-10-09 user).
bool GetViewCuesSetting();
void SetHeadAimForSave(bool on);

// Live settings (in-headset overlay): the [section] keys behind the simple getters above (and overlay.distance_m /
// width_m), as "section.key". Bools are 0/1, choices their index (render.mode: 0 sbs, 1 mono, 2 aer). SettingSet clamps
// to the key's range and marks it for saving; returns false for a key that isn't live.
float SettingGet(const char* key);
bool SettingSet(const char* key, float value);
float SettingDefault(const char* key);
// Every live value, and putting them back (the overlay's undo): keys whose value differs are set again (marked to save).
std::vector<float> SettingsSnapshot();
void SettingsRestore(const std::vector<float>& snapshot);
// Writes the keys changed by SettingSet into vrmod_settings.ini in place (comments and other keys kept); returns how
// many. SettingsWriteKeys does the same for any "section.key" -> value list.
int SettingsSaveChanged();
bool SettingsWriteKeys(const std::vector<std::pair<std::string, std::string>>& keys);
// The same for any ini file (e.g. Cheeky's CheekyFoveatedDLSS.ini).
bool IniWriteKeys(const char* file, const std::vector<std::pair<std::string, std::string>>& keys);
// The head camera settings, writable (overlay); GetHeadCamSettings returns the same object.
HeadCamSettings& HeadCamSettingsLive();

// First start of the mod: no vrmod_settings.ini yet. Writes it with every setting at its default (once) and returns true
// for the rest of the session (the overlay shows a welcome message).
bool SettingsFirstRun();

// Loads vrmod_settings.ini if present (missing keys keep their defaults).
Settings LoadSettings();

// Rewrites vrmod_settings.ini from the given settings.
void SaveSettings(const Settings& settings);

} // namespace vrmod
