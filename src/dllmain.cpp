#include <windows.h>
#include <mmsystem.h>
#include <string>

#pragma comment(lib, "winmm.lib")

#include "cheeky_loader.h"
#include "settings.h"
#include "logger.h"
#include "present_hook.h"
#include "d3d12_cb_hook.h"
#include "ansel_camera_hook.h"
#include "camera_interceptor.h"
#include "command_list_probe.h"
#include "stereo_mode.h"
#include "exit_hook.h"
#include "stall_watchdog.h"
#include "resolution_unlock.h"
#include "cinematic_watch.h"
#include "dlss_probe.h"
#include "rt_logger.h"
#include "cambone_probe.h"
#include "bone_probe.h"
#include "near_probe.h"
#include "ui_probe.h"
#include "d3d12_debug.h"

namespace {

bool g_inertHost = false;   // loaded into a process other than SOTTR.exe: exports only

// Frame pacing depends on timed waits (xrWaitFrame, fences) waking up on time.
// Windows 11 gives a process that isn't the foreground/visible window a coarse
// (~15.6 ms) timer and may throttle it, and a VR game's desktop window is
// exactly that. Opt out of all of it, and ask for 1 ms timers.
void ApplyTimingPolicy() {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;   // control bit set + state bit clear = "never throttle / honour timer requests"
    bool throttleOk = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != 0;
    bool timerOk = timeBeginPeriod(1) == TIMERR_NOERROR;
    bool prioOk = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != 0;
    modlog::Line(std::string("Timing policy: power-throttling opt-out ") + (throttleOk ? "ok" : "FAILED")
        + ", timeBeginPeriod(1) " + (timerOk ? "ok" : "FAILED") + ", above-normal priority " + (prioOk ? "ok" : "FAILED"));
}

DWORD WINAPI InitThread(LPVOID) {
    modlog::Init();
    modlog::Line("vrmod dxgi proxy loaded, starting init.");
    ApplyTimingPolicy();

    // Opt-in ([debug] d3d12_debug_layer=1). Has to happen before the game creates its D3D12 device, which it does
    // well after this thread starts (the proxy loads at process start).
    vrmod::D3D12DebugEnableIfRequested();

    // First, ahead of everything slow: the game enumerates display modes
    // within a second or so of starting, and the mode-list hooks and the
    // Surround-tag patch must be in place before that first pass. This part is
    // device-free (fast), so it goes before the throwaway-device work below.
    vrmod::SettingsFirstRun();   // a new install: vrmod_settings.ini with the defaults, before anything reads it
    if (!vrmod::InstallResolutionUnlock()) {
        modlog::Line("Resolution unlock installation failed (or partially failed).");
    }
    vrmod::InstallUiProbe();   // Brief 2 H0, only with [debug] ui_probe=1 (after the unlock: never delay it)

    if (!vrmod::InstallPresentHook()) {
        modlog::Line("Present hook installation failed; mod is inert but the game should still run normally.");
    }

    vrmod::InstallSwapChainWatchLate();

    // Disabled: the D3D12 constant-buffer probe intercepts tens of millions of
    // calls per minute (and logs them), which alone drops DX12 to ~1-3 fps.
    // Re-enable only for deliberate D3D12 investigation.
    // if (!vrmod::InstallD3D12ConstantBufferHook()) {
    //     modlog::Line("D3D12 constant buffer hook installation failed.");
    // }

    // The Ansel camera hook (phase 1 discovery) is no longer used, and at this point the exe may still be unpacking:
    // the hook always failed here, and on 2026-10-04 the game died silently right at it. Not installed.

    if (!vrmod::InstallCameraInterceptor()) {
        modlog::Line("Camera interceptor installation failed.");
    }

    // Cutscene detection: switches first-person off during in-engine cinematics and back on afterwards.
    vrmod::InstallCinematicWatch();
    vrmod::InstallDlssProbe();

    // Diagnostic (needs vrmod_rtlog.txt): render-target inventory, to compare SBS vs non-SBS passes.
    vrmod::InstallRenderTargetLogger();

    // Diagnostic: logs the joint index a bone-attached camera (dialogue/cutscene close-ups) is bound to.
    vrmod::InstallCameraBoneProbe();

    // Diagnostic: logs (instance handle, bone index) pairs the engine's generic bone-position accessor resolves
    // during normal play, to find Lara's own instance handle and her head bone's index empirically.
    vrmod::InstallBoneAccessorProbe();

    // First person, hiding the head: the per-camera value stored after the FOV (near-plane candidate), Ctrl+Alt+Z.
    vrmod::InstallNearProbe();

    // Disabled with the D3D12 probe above (same per-call overhead concern).
    // if (!vrmod::InstallCommandListProbe()) {
    //     modlog::Line("Command list probe installation failed.");
    // }

    if (!vrmod::InstallStereoModeProbe()) {
        modlog::Line("Stereo mode probe installation failed.");
    }

    if (!vrmod::InstallExitHook()) {
        modlog::Line("Exit hook installation failed.");
    }

    vrmod::StartStallWatchdog();

    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reasonForCall, LPVOID) {
    switch (reasonForCall) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);
        // Only inside the game. Its crash reporter (crashpad_handler.exe, started by SOTTR.exe from the same folder)
        // loads this dxgi.dll too: the mod's init then hooked addresses meant for SOTTR.exe inside it, it rotated and
        // truncated vrmod.log under the game's feet, and on 2026-10-04 a launch died with the reporter hung at init.
        // Elsewhere the proxy only forwards the dxgi exports (dxgi_exports.cpp loads the real DLL on first call).
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* base = wcsrchr(exe, L'\\');
        base = base ? base + 1 : exe;
        if (_wcsicmp(base, L"SOTTR.exe") != 0) {
            modlog::Close();   // no log file from other processes
            g_inertHost = true;
            break;
        }
        // Never call LoadLibrary / create D3D devices directly inside DllMain --
        // it runs under the loader lock and D3D11CreateDeviceAndSwapChain (used in
        // present_hook.cpp) can deadlock. Defer real init to a separate thread.
        CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
        // CheekyFoveatedDLSS, when installed and foveated rendering is on (cheeky_loader.cpp; its own worker thread).
        vrmod::CheekyLoaderStart();
        break;
    }

    case DLL_PROCESS_DETACH:
        if (g_inertHost) break;
        vrmod::RemovePresentHook();
        modlog::Line("vrmod dxgi proxy unloading.");
        modlog::Close();
        break;
    }
    return TRUE;
}
