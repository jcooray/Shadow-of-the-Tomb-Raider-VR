#pragma once

namespace vrmod {

// The game's own old msvcp140.dll made safe for OpenXR software built with a newer Visual Studio (crt_shim.cpp): once,
// before the OpenXR instance is created.
void CrtMutexShim();

}  // namespace vrmod
