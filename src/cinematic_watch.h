#pragma once

namespace vrmod {

// Detects the game's in-engine cutscenes ("cinematics") by watching its internal event bus.
// Ghidra showed the game posts two named events through FUN_1408e75b0(bus, &eventId, ...):
// evCinematicStarted (id stored at SOTTR.exe+0x35C9100) and evCinematicFinished (+0x35C90FC).
// Hooking the post function and comparing the id is cheap and needs no per-cutscene knowledge.
// On start the first-person camera is switched off (and remembered), on finish it is restored.
bool InstallCinematicWatch();
bool CinematicActive();
void EventMarkNow(const char* how);   // a MARK line in the log (in-headset overlay)
void EventMarkPollHotkey();   // J: MARK in the log (Present thread)

} // namespace vrmod
