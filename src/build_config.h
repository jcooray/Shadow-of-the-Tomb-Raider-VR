#pragma once

// VRMOD_DIAG: 1 = development build with the diagnostic probes (bone/camera/aim/gear probes, test patterns, stereo-mode
// writer, ...); 0 = release build without them (CMake option VRMOD_DIAGNOSTICS=OFF). With 0 the probes' sources aren't
// compiled and their headers turn every entry point into an inline no-op, so the core code calls them unchanged.
#ifndef VRMOD_DIAG
#define VRMOD_DIAG 1
#endif

// Keyboard in the release build (2026-10-19): only Home (first / third person), K (head aim), J (log mark) and Ctrl+Alt+O
// (the overlay) -- everything else is in the overlay, and the gamepad combos stay. Every other hotkey reads its key
// through VRMOD_DEV_KEY: GetAsyncKeyState in a development build, "not pressed" in a release build. The modifiers
// (Ctrl / Alt / Shift) and the kept keys are read with GetAsyncKeyState directly.
#if VRMOD_DIAG
#define VRMOD_DEV_KEY(vk) GetAsyncKeyState(vk)
#else
#define VRMOD_DEV_KEY(vk) (static_cast<void>(vk), static_cast<short>(0))
#endif
