#pragma once

#include "build_config.h"

#include <cstdint>

namespace vrmod {

// A0g (docs/AIM.md): who WRITES the camera orientation copies the aim finder found? After a finder run (Ctrl+Alt+Y),
// Ctrl+Alt+W arms hardware write watchpoints (DR0-DR3, write-only, 4 bytes) on up to four of its matches (one per
// object; angles first) on every thread for ~1.5 s. Each writing instruction is recorded with a count, the registers
// and XMM0-XMM5 at its first hit, and the SOTTR.exe return addresses found on the stack (the caller chain, useful when
// the writer is a memcpy). Writes vrmod_aimwatch_<time>.txt; the writers are then decompiled to trace the source.
// Present thread, once per frame.
#if VRMOD_DIAG
void AimWatchOnPresent();
// Arms the same watch on up to four addresses chosen by another probe (the facing probe, 2026-10-11). Present thread.
bool AimWatchStart(const uintptr_t* addrs, const char* const* labels, int count);
#else
inline void AimWatchOnPresent() {}
inline bool AimWatchStart(const uintptr_t*, const char* const*, int) { return false; }
#endif

}
