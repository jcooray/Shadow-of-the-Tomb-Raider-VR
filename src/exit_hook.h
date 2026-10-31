#pragma once

namespace vrmod {

// Hooks ExitProcess so the OpenXR session is destroyed *before* the process
// tears down the game's D3D11 device it's bound to. Without this the session
// is never destroyed and the game hung on exit (had to be force-killed).
// Also logs, with where they came from: the game terminating itself, crashes, and a start that hangs before the
// game's window appears (the main thread's stack).
bool InstallExitHook();

// From DllMain, which runs on the game's main thread: the thread the start-up hang check looks at.
void NoteMainThread();

} // namespace vrmod
