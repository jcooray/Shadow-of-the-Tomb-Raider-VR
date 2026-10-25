#pragma once

namespace vrmod {

// Hooks ExitProcess so the OpenXR session is destroyed *before* the process
// tears down the game's D3D11 device it's bound to. Without this the session
// is never destroyed and the game hung on exit (had to be force-killed).
bool InstallExitHook();

} // namespace vrmod
