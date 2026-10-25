#pragma once

namespace vrmod {

// Diagnostic for hangs: a background thread that logs, at most every few
// seconds, if Present hasn't been called for a while, together with the last
// phase marker set by the hooks. If a hang is inside our own code the marker
// names the call it's stuck in; if the marker says the game was between
// Presents, the stall is in the game or the runtime, not us.
void StartStallWatchdog();

// Call at the top of hkPresent.
void StallWatchdogNotePresent();

// `phase` must be a string literal (the pointer is stored, not copied).
void StallWatchdogSetPhase(const char* phase);

} // namespace vrmod
