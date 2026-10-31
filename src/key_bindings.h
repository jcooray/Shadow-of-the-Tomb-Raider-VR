#pragma once

#include <string>
#include <utility>
#include <vector>

namespace vrmod {

// The mod's keyboard shortcuts (1.0.4), each player's own: [keys] in vrmod_settings.ini ("overlay=Ctrl+Alt+O"), and the
// overlay's Controls page, where a line takes the next key pressed. The gamepad combos aren't affected.
enum class KeyAction { Overlay, Recentre, FirstPerson, HeadAim, Mark, Count };

struct KeyCombo {
    int vk = 0;   // 0 = none (the action has no key)
    bool ctrl = false, alt = false, shift = false;
    bool operator==(const KeyCombo& o) const { return vk == o.vk && ctrl == o.ctrl && alt == o.alt && shift == o.shift; }
};

const char* KeyActionLabel(KeyAction a);   // "Open / close the overlay"
KeyCombo KeyBinding(KeyAction a);
KeyCombo KeyDefault(KeyAction a);
std::string KeyText(const KeyCombo& k);    // "Ctrl+Alt+O", "none"

// Sets an action's key, saves it, and returns what to tell the player. A key another action has moves here (that one
// is left without a key); the overlay's key can't be taken away or be a plain key the overlay itself uses.
std::string KeyBindingSet(KeyAction a, const KeyCombo& k);

// The action's key is held, with exactly its modifiers. False while the overlay is open (except for the overlay's own
// key) and while a key is being chosen.
bool KeyActionDown(KeyAction a);
void KeyBindingsSetOverlayOpen(bool open);

// Choosing a key in the overlay: waits until every key is up, then takes the next one pressed with the modifiers held
// (Escape cancels). KeyCapturePoll returns true once it's done, with the action and the key, or cancelled.
void KeyCaptureStart(KeyAction a);
void KeyCaptureCancel();
bool KeyCaptureActive();
KeyAction KeyCaptureAction();
bool KeyCapturePoll(KeyAction* action, KeyCombo* key, bool* cancelled);

// "keys.<name>" -> text for every action (a whole-file settings save writes them back).
std::vector<std::pair<std::string, std::string>> KeyBindingsForSave();

} // namespace vrmod
