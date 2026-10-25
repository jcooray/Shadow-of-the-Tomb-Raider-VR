#pragma once

#include "build_config.h"

namespace vrmod {

// Diagnostic: logs every large D3D12 render target / depth / UAV texture the game creates (size, format, flags)
// so SBS and non-SBS renders can be compared pass by pass. Only installs if vrmod_rtlog.txt exists in the game
// folder; lines go to vrmod.log with an "RT " prefix.
#if VRMOD_DIAG
bool InstallRenderTargetLogger();
#else
inline bool InstallRenderTargetLogger() { return true; }
#endif

}
