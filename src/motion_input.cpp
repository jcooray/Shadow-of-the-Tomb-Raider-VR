#include "motion_input.h"

#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "eye_gaze.h"
#include "logger.h"
#include "overlay_pad.h"
#include "settings.h"

namespace vrmod {

namespace {

XrInstance g_inst = XR_NULL_HANDLE;
XrSession g_sess = XR_NULL_HANDLE;
XrActionSet g_gazeSet = XR_NULL_HANDLE;   // eye_gaze.cpp's, synced here
XrActionSet g_set = XR_NULL_HANDLE;       // the controllers'

enum Id {
    LeftStick, RightStick, LeftTrigger, RightTrigger, LeftGrip, RightGrip, A, B, X, Y, LeftClick, RightClick, Menu,
    LeftPad, LeftPadForce, RightPadForce, LeftPadTouch, LeftAim, RightAim, Count
};
XrAction g_action[Count] = {};

struct ActionDef {
    const char* name;
    XrActionType type;
};
const ActionDef kActions[Count] = {
    { "left_stick", XR_ACTION_TYPE_VECTOR2F_INPUT }, { "right_stick", XR_ACTION_TYPE_VECTOR2F_INPUT },
    { "left_trigger", XR_ACTION_TYPE_FLOAT_INPUT }, { "right_trigger", XR_ACTION_TYPE_FLOAT_INPUT },
    { "left_grip", XR_ACTION_TYPE_FLOAT_INPUT }, { "right_grip", XR_ACTION_TYPE_FLOAT_INPUT },
    { "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT }, { "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT },
    { "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT }, { "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT },
    { "left_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT }, { "right_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT },
    { "menu", XR_ACTION_TYPE_BOOLEAN_INPUT },
    { "left_trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT },
    { "left_trackpad_press", XR_ACTION_TYPE_FLOAT_INPUT }, { "right_trackpad_press", XR_ACTION_TYPE_FLOAT_INPUT },
    { "left_trackpad_touch", XR_ACTION_TYPE_BOOLEAN_INPUT },
    { "left_aim", XR_ACTION_TYPE_POSE_INPUT }, { "right_aim", XR_ACTION_TYPE_POSE_INPUT },
};

struct Binding {
    Id id;
    const char* path;
};
// Meta / Oculus Touch (Quest, Rift): menu on the left, X / Y on the left, no trackpads.
const Binding kTouch[] = {
    { LeftStick, "/user/hand/left/input/thumbstick" }, { RightStick, "/user/hand/right/input/thumbstick" },
    { LeftTrigger, "/user/hand/left/input/trigger/value" }, { RightTrigger, "/user/hand/right/input/trigger/value" },
    { LeftGrip, "/user/hand/left/input/squeeze/value" }, { RightGrip, "/user/hand/right/input/squeeze/value" },
    { A, "/user/hand/right/input/a/click" }, { B, "/user/hand/right/input/b/click" },
    { X, "/user/hand/left/input/x/click" }, { Y, "/user/hand/left/input/y/click" },
    { LeftClick, "/user/hand/left/input/thumbstick/click" }, { RightClick, "/user/hand/right/input/thumbstick/click" },
    { Menu, "/user/hand/left/input/menu/click" },
    { LeftAim, "/user/hand/left/input/aim/pose" }, { RightAim, "/user/hand/right/input/aim/pose" },
};
// Valve Index: A / B on both hands (the left's become X / Y), trackpads with force instead of a menu button. The grip
// is squeeze/force: squeeze/value is how far the fingers wrap the handle, high just from holding the controller
// (2026-10-31: LB and RB "held" the whole session).
const Binding kIndex[] = {
    { LeftStick, "/user/hand/left/input/thumbstick" }, { RightStick, "/user/hand/right/input/thumbstick" },
    { LeftTrigger, "/user/hand/left/input/trigger/value" }, { RightTrigger, "/user/hand/right/input/trigger/value" },
    { LeftGrip, "/user/hand/left/input/squeeze/force" }, { RightGrip, "/user/hand/right/input/squeeze/force" },
    { A, "/user/hand/right/input/a/click" }, { B, "/user/hand/right/input/b/click" },
    { X, "/user/hand/left/input/a/click" }, { Y, "/user/hand/left/input/b/click" },
    { LeftClick, "/user/hand/left/input/thumbstick/click" }, { RightClick, "/user/hand/right/input/thumbstick/click" },
    { LeftPad, "/user/hand/left/input/trackpad" },
    { LeftPadForce, "/user/hand/left/input/trackpad/force" }, { RightPadForce, "/user/hand/right/input/trackpad/force" },
    { LeftPadTouch, "/user/hand/left/input/trackpad/touch" },
    { LeftAim, "/user/hand/left/input/aim/pose" }, { RightAim, "/user/hand/right/input/aim/pose" },
};

bool g_ready = false;
XrResult g_lastSync = XR_SUCCESS;
XrSpace g_aimSpace[2] = {};   // the controllers' aim poses (stage 2: where the hand points)
std::mutex g_aimMutex;
float g_aimQ[2][4] = {}, g_aimP[2][3] = {};
bool g_aimValid[2] = {};
ULONGLONG g_aimAt[2] = {};

// What the actions read this frame.
struct Input {
    bool active = false;   // any controller action active (a profile is bound and the session is focused)
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0, lg = 0, rg = 0;
    bool a = false, b = false, x = false, y = false, lclick = false, rclick = false, menu = false;
    bool hasMenu = false, hasPads = false;   // which kind of controller is bound (Touch: menu; Index: trackpads)
    float padX = 0, padY = 0, lPadForce = 0, rPadForce = 0;
    bool padTouch = false;
};

int16_t Stick(float v) {
    if (!std::isfinite(v)) return 0;
    v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

uint8_t Trigger(float v) {
    if (!std::isfinite(v)) return 0;
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return static_cast<uint8_t>(std::lround(v * 255.0f));
}

// Controller input -> a gamepad state, with the chords for the buttons a pad has and the controllers don't (D-pad,
// View, Guide). Pulses (Menu on a tap, View on a long hold, a short stick click) last 120 ms.
class Mapper {
public:
    struct Out {
        XINPUT_GAMEPAD pad{};
        bool chord = false;   // the overlay chord is held
    };

    Out Update(const Input& in, ULONGLONG now) {
        Out o;
        if (!in.active) { *this = Mapper{}; return o; }
        XINPUT_GAMEPAD& p = o.pad;
        p.sThumbLX = Stick(in.lx); p.sThumbLY = Stick(in.ly);
        p.sThumbRX = Stick(in.rx); p.sThumbRY = Stick(in.ry);
        p.bLeftTrigger = Trigger(in.lt); p.bRightTrigger = Trigger(in.rt);
        WORD& b = p.wButtons;
        if (in.a) b |= XINPUT_GAMEPAD_A;
        if (in.b) b |= XINPUT_GAMEPAD_B;
        if (in.x) b |= XINPUT_GAMEPAD_X;
        if (in.y) b |= XINPUT_GAMEPAD_Y;
        lgDown_ = in.lg > (lgDown_ ? 0.35f : 0.55f);   // grips: a little hysteresis
        rgDown_ = in.rg > (rgDown_ ? 0.35f : 0.55f);
        if (lgDown_) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (rgDown_) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (in.lclick) b |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (in.hasPads) Index(in, now, o);
        else Touch(in, now, o);
        if (now < startUntil_) b |= XINPUT_GAMEPAD_START;
        if (now < backUntil_) b |= XINPUT_GAMEPAD_BACK;
        if (now < rsUntil_) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
        return o;
    }

    uint32_t chords = 0, dpads = 0, menus = 0, views = 0;   // for the log

private:
    // Index: right stick click is the real one. A swipe on the left trackpad = a D-pad press in its direction (half the
    // pad's width within 0.5 s; each swipe once, and a long one can go on to the next); pressing it = View, after 120 ms
    // so that a press of both trackpads together isn't View first. Right trackpad: a tap is Menu, held 1.5 s View too.
    // Both pressed together: the overlay chord, which uses up both presses.
    void Index(const Input& in, ULONGLONG now, Out& o) {
        if (in.rclick) o.pad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        const bool l = in.lPadForce > (lPad_ ? 0.25f : 0.45f);
        const bool r = in.rPadForce > (rPad_ ? 0.25f : 0.45f);
        if (l && !lPad_) lPadAt_ = now;
        if (r && !rPad_) { rPadAt_ = now; longFired_ = false; }
        if (l && r && !chord_) { chord_ = true; ++chords; }
        if (!l && !r) chord_ = false;
        o.chord = chord_ && l && r;
        if (!chord_) {
            if (l && now - lPadAt_ >= 120) {
                if (!viewHeld_) ++views;
                viewHeld_ = true;
                o.pad.wButtons |= XINPUT_GAMEPAD_BACK;
            }
            if (r && !longFired_ && now - rPadAt_ >= 1500) { longFired_ = true; backUntil_ = now + 120; ++views; }
            if (!r && rPad_ && !longFired_ && !chordSeen_) { startUntil_ = now + 120; ++menus; }
        }
        if (!l) viewHeld_ = false;
        Swipe(in, now, l);
        if (now < swipeUntil_) o.pad.wButtons |= swipeDir_;
        // A press that was part of the chord does nothing when it's released.
        if (chord_) chordSeen_ = true;
        if (!l && !r) chordSeen_ = false;
        lPad_ = l;
        rPad_ = r;
    }

    void Swipe(const Input& in, ULONGLONG now, bool pressed) {
        const bool touch = in.padTouch && !pressed;   // a press is View, not a swipe
        if (touch && !swipeTouch_) { swipeX_ = in.padX; swipeY_ = in.padY; swipeAt_ = now; }
        swipeTouch_ = touch;
        if (!touch) return;
        if (now - swipeAt_ > 500) { swipeX_ = in.padX; swipeY_ = in.padY; swipeAt_ = now; return; }   // a slow drift
        const float dx = in.padX - swipeX_, dy = in.padY - swipeY_;
        const float ax = std::fabs(dx), ay = std::fabs(dy);
        if (ax < 0.6f && ay < 0.6f) return;
        if (ay >= ax) swipeDir_ = dy > 0 ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN;
        else swipeDir_ = dx > 0 ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
        swipeUntil_ = now + 150;
        ++dpads;
        swipeX_ = in.padX;
        swipeY_ = in.padY;
        swipeAt_ = now;
    }

    // Touch: left Menu, a tap = Menu, held 1.5 s = View. Right stick click held + left stick = D-pad (the left stick then
    // doesn't walk); a short click on its own is the stick click, and one held 0.35 s without a D-pad direction is the
    // stick click held (LB + right stick held = aim where you look; the left stick walks again). A click while walking
    // (the left stick already pushed) is the stick click at once, and walking goes on. Menu + right stick click: the
    // overlay chord.
    void Touch(const Input& in, ULONGLONG now, Out& o) {
        if (in.menu && in.rclick && !chord_) { chord_ = true; ++chords; }
        if (!in.menu && !in.rclick) chord_ = false;
        o.chord = chord_ && in.menu && in.rclick;
        if (in.rclick && !rclick_) {
            rclickAt_ = now;
            dpadUsed_ = false;
            rsHeld_ = std::fabs(in.lx) >= 0.3f || std::fabs(in.ly) >= 0.3f;   // walking: a plain stick click
        }
        if (in.rclick && !chord_ && !dpadUsed_ && now - rclickAt_ >= 350) rsHeld_ = true;
        if (in.rclick && rsHeld_) {
            o.pad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        } else if (in.rclick && !chord_) {
            o.pad.sThumbLX = o.pad.sThumbLY = 0;
            const float ax = std::fabs(in.lx), ay = std::fabs(in.ly);
            if (ax >= 0.5f || ay >= 0.5f) {
                if (!dpadUsed_) ++dpads;
                dpadUsed_ = true;
                if (ay >= ax) o.pad.wButtons |= in.ly > 0 ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN;
                else o.pad.wButtons |= in.lx > 0 ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
            }
        } else if (rclick_ && !in.rclick && !dpadUsed_ && !chordSeen_ && now - rclickAt_ < 350) {
            rsUntil_ = now + 120;
        }
        rclick_ = in.rclick;
        if (in.menu && !menu_) { menuAt_ = now; longFired_ = false; }
        if (!chord_ && in.menu && !longFired_ && now - menuAt_ >= 1500) { longFired_ = true; backUntil_ = now + 120; ++views; }
        if (!chord_ && !in.menu && menu_ && !longFired_ && !chordSeen_) { startUntil_ = now + 120; ++menus; }
        menu_ = in.menu;
        if (chord_) chordSeen_ = true;
        if (!in.menu && !in.rclick) chordSeen_ = false;
    }

    bool lgDown_ = false, rgDown_ = false;
    bool chord_ = false, chordSeen_ = false, longFired_ = false;
    bool lPad_ = false, rPad_ = false, viewHeld_ = false;
    bool swipeTouch_ = false;
    float swipeX_ = 0, swipeY_ = 0;
    ULONGLONG swipeAt_ = 0, swipeUntil_ = 0;
    WORD swipeDir_ = 0;
    ULONGLONG lPadAt_ = 0, rPadAt_ = 0;
    bool rclick_ = false, dpadUsed_ = false, rsHeld_ = false, menu_ = false;
    ULONGLONG rclickAt_ = 0, menuAt_ = 0;
    ULONGLONG startUntil_ = 0, backUntil_ = 0, rsUntil_ = 0;
};

Mapper g_mapper;

std::mutex g_padMutex;
XINPUT_GAMEPAD g_pad{};
bool g_chord = false;
bool g_padActive = false;
ULONGLONG g_padAt = 0;
std::atomic<bool> g_everUsed{ false };
std::atomic<uint64_t> g_delivered[2] = {};

// For the 10 s log line.
uint64_t g_frames = 0, g_activeFrames = 0, g_notFocused = 0, g_background = 0;
WORD g_buttonsSeen = 0;

std::string ProfileOf(const char* hand) {
    XrPath path = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(g_inst, hand, &path))) return "?";
    XrInteractionProfileState st{ XR_TYPE_INTERACTION_PROFILE_STATE };
    if (XR_FAILED(xrGetCurrentInteractionProfile(g_sess, path, &st)) || st.interactionProfile == XR_NULL_PATH) return "none";
    char name[XR_MAX_PATH_LENGTH] = {};
    uint32_t n = 0;
    xrPathToString(g_inst, st.interactionProfile, sizeof(name), &n, name);
    return name;
}

bool Suggest(const char* profile, const Binding* b, size_t count) {
    std::vector<XrActionSuggestedBinding> list;
    for (size_t i = 0; i < count; ++i) {
        XrPath path = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(g_inst, b[i].path, &path))) continue;
        list.push_back({ g_action[b[i].id], path });
    }
    XrPath prof = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(g_inst, profile, &prof))) return false;
    XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = prof;
    sb.countSuggestedBindings = static_cast<uint32_t>(list.size());
    sb.suggestedBindings = list.data();
    const XrResult r = xrSuggestInteractionProfileBindings(g_inst, &sb);
    if (XR_FAILED(r)) modlog::Line(std::string("MOTION: the runtime refused the bindings for ") + profile + " (" + std::to_string(static_cast<int>(r)) + ")");
    return XR_SUCCEEDED(r);
}

bool CreateControllerActions() {
    XrActionSetCreateInfo si{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(si.actionSetName, "vrmod_controllers");
    strcpy_s(si.localizedActionSetName, "VR mod motion controllers");
    if (XR_FAILED(xrCreateActionSet(g_inst, &si, &g_set))) { modlog::Line("MOTION: xrCreateActionSet failed"); return false; }
    for (int i = 0; i < Count; ++i) {
        XrActionCreateInfo ai{ XR_TYPE_ACTION_CREATE_INFO };
        ai.actionType = kActions[i].type;
        strcpy_s(ai.actionName, kActions[i].name);
        strcpy_s(ai.localizedActionName, kActions[i].name);
        if (XR_FAILED(xrCreateAction(g_set, &ai, &g_action[i]))) {
            modlog::Line(std::string("MOTION: xrCreateAction failed for ") + kActions[i].name);
            xrDestroyActionSet(g_set);
            g_set = XR_NULL_HANDLE;
            return false;
        }
    }
    const bool touch = Suggest("/interaction_profiles/oculus/touch_controller", kTouch, sizeof(kTouch) / sizeof(kTouch[0]));
    const bool index = Suggest("/interaction_profiles/valve/index_controller", kIndex, sizeof(kIndex) / sizeof(kIndex[0]));
    return touch || index;
}

bool ReadBool(Id id, bool* any) {
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = g_action[id];
    XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(g_sess, &gi, &st)) || !st.isActive) return false;
    *any = true;
    return st.currentState != XR_FALSE;
}

float ReadFloat(Id id, bool* any, bool* active = nullptr) {
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = g_action[id];
    XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(g_sess, &gi, &st)) || !st.isActive) return 0.0f;
    *any = true;
    if (active) *active = true;
    return st.currentState;
}

void ReadVec(Id id, bool* any, float* x, float* y, bool* active = nullptr) {
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = g_action[id];
    XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(g_sess, &gi, &st)) || !st.isActive) return;
    *any = true;
    if (active) *active = true;
    *x = st.currentState.x;
    *y = st.currentState.y;
}

bool GameInFront() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void Publish(const Mapper::Out& o, bool active, ULONGLONG now) {
    std::lock_guard<std::mutex> lock(g_padMutex);
    g_pad = o.pad;
    g_chord = o.chord;
    g_padActive = active;
    g_padAt = now;
}

void LogEvery10s(ULONGLONG now) {
    static ULONGLONG last = 0;
    static std::string profiles;
    static ULONGLONG lastProfileCheck = 0;
    if (now - lastProfileCheck >= 2000) {   // the controllers bound can change (switched on, swapped)
        lastProfileCheck = now;
        const std::string p = "left " + ProfileOf("/user/hand/left") + ", right " + ProfileOf("/user/hand/right");
        if (p != profiles) {
            profiles = p;
            modlog::Line("MOTION: controllers now: " + p);
        }
    }
    if (now - last < 10000) return;
    last = now;
    if (!g_activeFrames && !g_everUsed.load()) { g_frames = g_notFocused = g_background = 0; return; }
    std::string seen;
    const struct { WORD bit; const char* name; } kNames[] = {
        { XINPUT_GAMEPAD_A, "A" }, { XINPUT_GAMEPAD_B, "B" }, { XINPUT_GAMEPAD_X, "X" }, { XINPUT_GAMEPAD_Y, "Y" },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, "LB" }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB" }, { XINPUT_GAMEPAD_LEFT_THUMB, "LS" },
        { XINPUT_GAMEPAD_RIGHT_THUMB, "RS" }, { XINPUT_GAMEPAD_START, "Menu" }, { XINPUT_GAMEPAD_BACK, "View" },
        { XINPUT_GAMEPAD_DPAD_UP, "Up" }, { XINPUT_GAMEPAD_DPAD_DOWN, "Down" }, { XINPUT_GAMEPAD_DPAD_LEFT, "Left" },
        { XINPUT_GAMEPAD_DPAD_RIGHT, "Right" },
    };
    for (const auto& n : kNames) if (g_buttonsSeen & n.bit) seen += std::string(seen.empty() ? "" : " ") + n.name;
    char b[400];
    snprintf(b, sizeof(b), "MOTION (10 s): %llu frames, controllers in use %llu, not focused %llu, game not in front %llu | buttons %s | "
             "D-pad %u, Menu %u, View %u, overlay chord %u | the game's reads with the virtual pad: XInput %llu, Windows.Gaming.Input %llu",
             static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_activeFrames),
             static_cast<unsigned long long>(g_notFocused), static_cast<unsigned long long>(g_background),
             seen.empty() ? "none" : seen.c_str(), g_mapper.dpads, g_mapper.menus, g_mapper.views, g_mapper.chords,
             static_cast<unsigned long long>(g_delivered[0].exchange(0)), static_cast<unsigned long long>(g_delivered[1].exchange(0)));
    modlog::Line(b);
    g_frames = g_activeFrames = g_notFocused = g_background = 0;
    g_buttonsSeen = 0;
    g_mapper.dpads = g_mapper.menus = g_mapper.views = g_mapper.chords = 0;
}

} // namespace

void MotionInputInitSession(void* instance, void* session, uint64_t systemId, bool eyeGaze) {
    g_inst = static_cast<XrInstance>(instance);
    g_sess = static_cast<XrSession>(session);
    g_ready = false;
    g_gazeSet = eyeGaze ? static_cast<XrActionSet>(EyeGazeCreateActions(instance, session, systemId)) : XR_NULL_HANDLE;
    if (!eyeGaze) EyeGazeNotOffered();
    const bool controllers = CreateControllerActions();
    std::vector<XrActionSet> sets;
    if (g_gazeSet) sets.push_back(g_gazeSet);
    if (controllers) sets.push_back(g_set);
    if (sets.empty()) return;
    XrSessionActionSetsAttachInfo at{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = static_cast<uint32_t>(sets.size());
    at.actionSets = sets.data();
    XrResult r = xrAttachSessionActionSets(g_sess, &at);
    if (XR_FAILED(r) && g_gazeSet && controllers) {   // a runtime that rejects the controllers mustn't cost the eye gaze
        modlog::Line("MOTION: attaching the controller actions failed (" + std::to_string(static_cast<int>(r)) + ") -- eye gaze only");
        at.countActionSets = 1;
        at.actionSets = &g_gazeSet;
        r = xrAttachSessionActionSets(g_sess, &at);
        xrDestroyActionSet(g_set);
        g_set = XR_NULL_HANDLE;
    }
    if (XR_FAILED(r)) {
        modlog::Line("MOTION: xrAttachSessionActionSets failed (" + std::to_string(static_cast<int>(r)) + ")");
        return;
    }
    if (g_gazeSet) EyeGazeActionsAttached();
    if (g_set) {
        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo asi{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
            asi.action = g_action[h ? RightAim : LeftAim];
            asi.poseInActionSpace.orientation.w = 1.0f;
            if (XR_FAILED(xrCreateActionSpace(g_sess, &asi, &g_aimSpace[h]))) g_aimSpace[h] = XR_NULL_HANDLE;
        }
    }
    g_ready = true;
    modlog::Line(std::string("MOTION: ") + (g_set ? "controller actions ready (Index and Touch bindings suggested)" : "no controller actions")
                 + (g_gazeSet ? ", attached with eye gaze's" : ""));
}

// The controllers' aim poses in the eyes' space (the views are located in it too), for the display time.
void LocateAims(void* appSpace, int64_t time) {
    for (int h = 0; h < 2; ++h) {
        bool ok = false;
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        if (g_aimSpace[h] && appSpace && XR_SUCCEEDED(xrLocateSpace(g_aimSpace[h], static_cast<XrSpace>(appSpace), time, &loc))) {
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            ok = (loc.locationFlags & need) == need;
        }
        std::lock_guard<std::mutex> lock(g_aimMutex);
        g_aimValid[h] = ok;
        if (!ok) continue;
        const XrQuaternionf& q = loc.pose.orientation;
        const XrVector3f& v = loc.pose.position;
        g_aimQ[h][0] = q.x; g_aimQ[h][1] = q.y; g_aimQ[h][2] = q.z; g_aimQ[h][3] = q.w;
        g_aimP[h][0] = v.x; g_aimP[h][1] = v.y; g_aimP[h][2] = v.z;
        g_aimAt[h] = GetTickCount64();
    }
}

void MotionInputFrame(int64_t predictedDisplayTime, void* appSpace) {
    XrResult sync = XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (g_ready) {
        XrActiveActionSet active[2] = {};
        uint32_t n = 0;
        if (g_gazeSet) active[n++] = { g_gazeSet, XR_NULL_PATH };
        if (g_set) active[n++] = { g_set, XR_NULL_PATH };
        XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
        si.countActiveActionSets = n;
        si.activeActionSets = active;
        sync = xrSyncActions(g_sess, &si);
        g_lastSync = sync;
    }
    EyeGazeUpdate(predictedDisplayTime, static_cast<int>(sync));
    if (!g_set) return;
    LocateAims(sync == XR_SUCCESS ? appSpace : nullptr, predictedDisplayTime);

    const ULONGLONG now = GetTickCount64();
    ++g_frames;
    Input in;
    const bool focused = sync == XR_SUCCESS;
    const bool front = GameInFront();
    const bool wanted = SettingGet("motion.controllers") != 0.0f;
    if (!focused) ++g_notFocused;
    else if (!front) ++g_background;
    if (focused && front && wanted) {
        bool any = false;
        ReadVec(LeftStick, &any, &in.lx, &in.ly);
        ReadVec(RightStick, &any, &in.rx, &in.ry);
        in.lt = ReadFloat(LeftTrigger, &any);
        in.rt = ReadFloat(RightTrigger, &any);
        in.lg = ReadFloat(LeftGrip, &any);
        in.rg = ReadFloat(RightGrip, &any);
        in.a = ReadBool(A, &any);
        in.b = ReadBool(B, &any);
        in.x = ReadBool(X, &any);
        in.y = ReadBool(Y, &any);
        in.lclick = ReadBool(LeftClick, &any);
        in.rclick = ReadBool(RightClick, &any);
        {
            bool menuActive = false;
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = g_action[Menu];
            XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(xrGetActionStateBoolean(g_sess, &gi, &st)) && st.isActive) { menuActive = true; any = true; in.menu = st.currentState != XR_FALSE; }
            in.hasMenu = menuActive;
        }
        ReadVec(LeftPad, &any, &in.padX, &in.padY, &in.hasPads);
        in.lPadForce = ReadFloat(LeftPadForce, &any, &in.hasPads);
        in.rPadForce = ReadFloat(RightPadForce, &any, &in.hasPads);
        in.padTouch = ReadBool(LeftPadTouch, &any);
        in.active = any;
    }
    const Mapper::Out o = g_mapper.Update(in, now);
    if (in.active) {
        ++g_activeFrames;
        g_buttonsSeen |= o.pad.wButtons;
        if (!g_everUsed.exchange(true)) modlog::Line("MOTION: controllers in use -- the game gets them as a gamepad (XInput slot 0 connected from now on)");
    }
    Publish(o, in.active, now);
    LogEvery10s(now);
}

void MotionInputShutdown() {
    for (XrSpace& sp : g_aimSpace) if (sp) { xrDestroySpace(sp); sp = XR_NULL_HANDLE; }
    if (g_set) { xrDestroyActionSet(g_set); g_set = XR_NULL_HANDLE; }
    for (XrAction& a : g_action) a = XR_NULL_HANDLE;
    g_gazeSet = XR_NULL_HANDLE;   // eye_gaze.cpp destroys its own
    g_ready = false;
    Publish(Mapper::Out{}, false, GetTickCount64());
}

bool MotionPadGame(XINPUT_GAMEPAD* out) {
    std::lock_guard<std::mutex> lock(g_padMutex);
    if (!g_padActive || GetTickCount64() - g_padAt > 250) return false;
    *out = g_pad;
    if (g_chord) out->wButtons &= ~XINPUT_GAMEPAD_START;
    return true;
}

bool MotionPadOverlay(XINPUT_GAMEPAD* out) {
    std::lock_guard<std::mutex> lock(g_padMutex);
    if (!g_padActive || GetTickCount64() - g_padAt > 250) return false;
    *out = g_pad;
    if (g_chord) out->wButtons |= kPadGuide | XINPUT_GAMEPAD_START;
    return true;
}

bool MotionHandAim(int hand, float q[4], float p[3]) {
    if (hand != 0 && hand != 1) return false;
    std::lock_guard<std::mutex> lock(g_aimMutex);
    if (!g_aimValid[hand] || GetTickCount64() - g_aimAt[hand] > 250) return false;
    for (int k = 0; k < 4; ++k) q[k] = g_aimQ[hand][k];
    for (int k = 0; k < 3; ++k) p[k] = g_aimP[hand][k];
    return true;
}

bool MotionPadEverUsed() {
    return g_everUsed.load();
}

void MotionPadNoteDelivered(int api) {
    if (api == 0 || api == 1) g_delivered[api].fetch_add(1, std::memory_order_relaxed);
}

} // namespace vrmod
