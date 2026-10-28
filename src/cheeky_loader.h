#pragma once
// CheekyFoveatedDLSS (optional foveated rendering) started by this dxgi.dll -- what Cheeky's own version.dll loader did,
// so that loader isn't installed (2026-10-28: as a proxy DLL loading another, it drew 8/71 virus-scanner detections;
// this dxgi.dll none). Only in SOTTR.exe, only with the CheekyFoveatedDLSS folder installed and foveated rendering on.

namespace vrmod {

// DllMain: a worker thread loads CheekyFoveatedDLSS\CheekyFoveatedDLSSHost.dll and calls its CheekyHost_Start(1)
// (standalone host) -- off the loader lock, as Cheeky's loader does.
void CheekyLoaderStart();

// After a successful real CreateDXGIFactory*: waits (once, up to 10 s) for the worker, so Cheeky's hooks are in before
// the game creates its first swap chain. Not on the worker's own thread (Cheeky creates factories while starting).
void CheekyLoaderAfterFactory();

}
