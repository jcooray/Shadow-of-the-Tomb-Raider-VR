#include "key_bindings.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>

#include "logger.h"
#include "settings.h"

namespace vrmod {

namespace {

struct ActionInfo {
    const char* key;     // in [keys]
    const char* label;   // on the overlay's Controls page
    KeyCombo def;
};

const ActionInfo kActions[] = {
    { "overlay", "Open / close the overlay", { 'O', true, true, false } },
    { "recentre", "Recentre the view", { 'R', true, true, false } },
    { "first_person", "First / third person", { VK_HOME, false, false, false } },
    { "head_aim", "Aim where you look", { 'K', false, false, false } },
    { "mark", "Mark the moment in the log", { 'J', false, false, false } },
};
constexpr int kCount = static_cast<int>(KeyAction::Count);
static_assert(sizeof(kActions) / sizeof(kActions[0]) == kCount, "one entry per action");

struct KeyName {
    int vk;
    const char* name;
};

// The keys a shortcut can use, by the names the ini takes. Letters and digits are added in code.
const KeyName kNames[] = {
    { VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" }, { VK_F5, "F5" }, { VK_F6, "F6" },
    { VK_F7, "F7" }, { VK_F8, "F8" }, { VK_F9, "F9" }, { VK_F10, "F10" }, { VK_F11, "F11" }, { VK_F12, "F12" },
    { VK_F13, "F13" }, { VK_F14, "F14" }, { VK_F15, "F15" }, { VK_F16, "F16" }, { VK_F17, "F17" }, { VK_F18, "F18" },
    { VK_F19, "F19" }, { VK_F20, "F20" }, { VK_F21, "F21" }, { VK_F22, "F22" }, { VK_F23, "F23" }, { VK_F24, "F24" },
    { VK_HOME, "Home" }, { VK_END, "End" }, { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" }, { VK_INSERT, "Insert" },
    { VK_DELETE, "Delete" }, { VK_UP, "Up" }, { VK_DOWN, "Down" }, { VK_LEFT, "Left" }, { VK_RIGHT, "Right" },
    { VK_SPACE, "Space" }, { VK_TAB, "Tab" }, { VK_RETURN, "Enter" }, { VK_BACK, "Backspace" }, { VK_PAUSE, "Pause" },
    { VK_SCROLL, "ScrollLock" },
    { VK_NUMPAD0, "Numpad0" }, { VK_NUMPAD1, "Numpad1" }, { VK_NUMPAD2, "Numpad2" }, { VK_NUMPAD3, "Numpad3" },
    { VK_NUMPAD4, "Numpad4" }, { VK_NUMPAD5, "Numpad5" }, { VK_NUMPAD6, "Numpad6" }, { VK_NUMPAD7, "Numpad7" },
    { VK_NUMPAD8, "Numpad8" }, { VK_NUMPAD9, "Numpad9" }, { VK_ADD, "NumpadPlus" }, { VK_SUBTRACT, "NumpadMinus" },
    { VK_MULTIPLY, "NumpadMultiply" }, { VK_DIVIDE, "NumpadDivide" }, { VK_DECIMAL, "NumpadDecimal" },
    { VK_OEM_MINUS, "Minus" }, { VK_OEM_PLUS, "Equals" }, { VK_OEM_4, "LeftBracket" }, { VK_OEM_6, "RightBracket" },
    { VK_OEM_1, "Semicolon" }, { VK_OEM_7, "Quote" }, { VK_OEM_COMMA, "Comma" }, { VK_OEM_PERIOD, "Period" },
    { VK_OEM_2, "Slash" }, { VK_OEM_5, "Backslash" }, { VK_OEM_3, "Backtick" },
};
// Other names the ini accepts for the same keys.
const KeyName kAliases[] = {
    { VK_PRIOR, "PgUp" }, { VK_NEXT, "PgDn" }, { VK_DELETE, "Del" }, { VK_INSERT, "Ins" }, { VK_RETURN, "Return" },
};

// The keys the overlay itself uses without modifiers: its own key can't be one of them.
bool OverlayNavigationKey(int vk) {
    return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN || vk == VK_BACK
        || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME;
}

const char* NameOf(int vk) {
    for (const KeyName& k : kNames) if (k.vk == vk) return k.name;
    return nullptr;
}

bool Usable(int vk) {
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || NameOf(vk) != nullptr;
}

bool SameText(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return i == a.size() && !b[i];
}

std::string Trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool Parse(const std::string& text, KeyCombo* out) {
    KeyCombo k;
    const std::string all = Trimmed(text);
    if (SameText(all, "none")) { *out = k; return true; }
    size_t start = 0;
    while (start <= all.size()) {
        const size_t plus = all.find('+', start);
        const std::string part = Trimmed(all.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        const bool last = plus == std::string::npos;
        if (SameText(part, "Ctrl") || SameText(part, "Control")) k.ctrl = true;
        else if (SameText(part, "Alt")) k.alt = true;
        else if (SameText(part, "Shift")) k.shift = true;
        else if (!last || k.vk) return false;   // the key itself comes last, once
        else if (part.size() == 1 && std::isalnum(static_cast<unsigned char>(part[0]))) k.vk = std::toupper(static_cast<unsigned char>(part[0]));
        else {
            for (const KeyName& n : kNames) if (SameText(part, n.name)) k.vk = n.vk;
            for (const KeyName& n : kAliases) if (SameText(part, n.name)) k.vk = n.vk;
            if (!k.vk) return false;
        }
        if (last) break;
        start = plus + 1;
    }
    if (!k.vk) return false;
    *out = k;
    return true;
}

uint32_t Pack(const KeyCombo& k) {
    return static_cast<uint32_t>(k.vk & 0xFF) | (k.ctrl ? 0x100u : 0) | (k.alt ? 0x200u : 0) | (k.shift ? 0x400u : 0);
}

KeyCombo Unpack(uint32_t v) {
    KeyCombo k;
    k.vk = static_cast<int>(v & 0xFF);
    k.ctrl = (v & 0x100) != 0;
    k.alt = (v & 0x200) != 0;
    k.shift = (v & 0x400) != 0;
    return k;
}

std::atomic<uint32_t> g_bind[kCount];
std::once_flag g_loadOnce;
std::mutex g_setMutex;
std::atomic<bool> g_overlayOpen{ false };

// Choosing a key: 0 off, 1 waiting for every key to be up, 2 taking the next key.
std::atomic<int> g_capture{ 0 };
std::atomic<int> g_captureAction{ 0 };
bool g_wasDown[256] = {};

bool Down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

std::string IniKey(int i) {
    return std::string("keys.") + kActions[i].key;
}

std::vector<std::pair<std::string, std::string>> ForSave() {
    std::vector<std::pair<std::string, std::string>> out;
    for (int i = 0; i < kCount; ++i) out.emplace_back(IniKey(i), KeyText(Unpack(g_bind[i].load())));
    return out;
}

void Load() {
    std::call_once(g_loadOnce, [] {
        bool missing = false;
        std::string summary;
        for (int i = 0; i < kCount; ++i) {
            const std::string text = SettingString(IniKey(i).c_str());
            KeyCombo k = kActions[i].def;
            if (text.empty()) {
                missing = true;
            } else if (!Parse(text, &k) || (i == static_cast<int>(KeyAction::Overlay) && !k.vk)) {
                k = kActions[i].def;
                modlog::Line(std::string("KEYS: [keys] ") + kActions[i].key + "=" + text + " isn't a key the mod knows -- "
                             + KeyText(k) + " instead (key names: README, Controls).");
            }
            g_bind[i].store(Pack(k));
            summary += std::string(i ? ", " : "") + kActions[i].key + " " + KeyText(k);
        }
        modlog::Line("KEYS: " + summary);
        if (missing) SettingsWriteKeys(ForSave());   // the [keys] section with every key, to edit by hand
    });
}

} // namespace

const char* KeyActionLabel(KeyAction a) {
    return kActions[static_cast<int>(a)].label;
}

KeyCombo KeyBinding(KeyAction a) {
    Load();
    return Unpack(g_bind[static_cast<int>(a)].load());
}

KeyCombo KeyDefault(KeyAction a) {
    return kActions[static_cast<int>(a)].def;
}

std::string KeyText(const KeyCombo& k) {
    if (!k.vk) return "none";
    std::string s;
    if (k.ctrl) s += "Ctrl+";
    if (k.alt) s += "Alt+";
    if (k.shift) s += "Shift+";
    if ((k.vk >= 'A' && k.vk <= 'Z') || (k.vk >= '0' && k.vk <= '9')) s += static_cast<char>(k.vk);
    else if (const char* n = NameOf(k.vk)) s += n;
    else s += "?";
    return s;
}

std::string KeyBindingSet(KeyAction a, const KeyCombo& k) {
    Load();
    const int i = static_cast<int>(a);
    const KeyAction overlay = KeyAction::Overlay;
    std::string note;
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        if (a == overlay && !k.vk) return "the overlay always needs a key";
        if (a == overlay && !k.ctrl && !k.alt && !k.shift && OverlayNavigationKey(k.vk)) {
            return KeyText(k) + " moves around in the overlay -- add Ctrl or Alt";
        }
        if (k.vk) {
            for (int j = 0; j < kCount; ++j) {
                if (j == i || !(Unpack(g_bind[j].load()) == k)) continue;
                if (j == static_cast<int>(overlay)) return KeyText(k) + " already opens the overlay";
                g_bind[j].store(0);
                note = " (" + std::string(kActions[j].label) + " has no key now)";
            }
        }
        g_bind[i].store(Pack(k));
    }
    SettingsWriteKeys(ForSave());
    modlog::Line(std::string("KEYS: ") + kActions[i].key + " = " + KeyText(k) + note);
    return std::string(kActions[i].label) + ": " + KeyText(k) + note;
}

bool KeyActionDown(KeyAction a) {
    if (g_capture.load() || (g_overlayOpen.load() && a != KeyAction::Overlay)) return false;
    const KeyCombo k = KeyBinding(a);
    return k.vk && Down(k.vk) && Down(VK_CONTROL) == k.ctrl && Down(VK_MENU) == k.alt && Down(VK_SHIFT) == k.shift;
}

void KeyBindingsSetOverlayOpen(bool open) {
    g_overlayOpen.store(open);
}

void KeyCaptureStart(KeyAction a) {
    g_captureAction.store(static_cast<int>(a));
    g_capture.store(1);
}

void KeyCaptureCancel() {
    g_capture.store(0);
}

bool KeyCaptureActive() {
    return g_capture.load() != 0;
}

KeyAction KeyCaptureAction() {
    return static_cast<KeyAction>(g_captureAction.load());
}

bool KeyCapturePoll(KeyAction* action, KeyCombo* key, bool* cancelled) {
    const int state = g_capture.load();
    if (!state) return false;
    if (state == 1) {   // the key that chose the line (Enter) and anything else still held must come up first
        for (int vk = 8; vk < 256; ++vk) {
            if (Down(vk) && vk != VK_CONTROL && vk != VK_MENU && vk != VK_SHIFT && vk != VK_LCONTROL && vk != VK_RCONTROL
                && vk != VK_LMENU && vk != VK_RMENU && vk != VK_LSHIFT && vk != VK_RSHIFT) {
                return false;
            }
        }
        for (int vk = 0; vk < 256; ++vk) g_wasDown[vk] = false;
        g_capture.store(2);
        return false;
    }
    *action = KeyCaptureAction();
    if (Down(VK_ESCAPE)) {
        g_capture.store(0);
        *cancelled = true;
        return true;
    }
    for (int vk = 8; vk < 256; ++vk) {
        const bool d = Down(vk);
        const bool pressed = d && !g_wasDown[vk];
        g_wasDown[vk] = d;
        if (!pressed || !Usable(vk)) continue;
        key->vk = vk;
        key->ctrl = Down(VK_CONTROL);
        key->alt = Down(VK_MENU);
        key->shift = Down(VK_SHIFT);
        g_capture.store(0);
        *cancelled = false;
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> KeyBindingsForSave() {
    Load();
    return ForSave();
}

} // namespace vrmod
