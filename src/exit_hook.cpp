#include "exit_hook.h"

#include <windows.h>
#include <MinHook.h>

#include "logger.h"
#include "openxr_integration.h"
#include "frame_log.h"

namespace vrmod {

namespace {

using ExitProcess_t = VOID(WINAPI*)(UINT);
ExitProcess_t oExitProcess = nullptr;

VOID WINAPI hkExitProcess(UINT exitCode) {
    modlog::Line("ExitProcess called, tearing OpenXR down first.");
    FrameLogShutdown();   // writes the per-frame CSVs if a recording is running
    ShutdownOpenXR();
    oExitProcess(exitCode);
}

} // namespace

bool InstallExitHook() {
    // MH_Initialize() already called by InstallPresentHook() before this runs.
    void* target = nullptr;
    if (MH_CreateHookApiEx(L"kernel32", "ExitProcess", &hkExitProcess, reinterpret_cast<void**>(&oExitProcess), &target) != MH_OK
        || MH_EnableHook(target) != MH_OK) {
        modlog::Line("ExitHook: ExitProcess hook setup failed.");
        return false;
    }
    modlog::Line("ExitHook: installed.");
    return true;
}

} // namespace vrmod
