#include "vr_overlay.h"
#include "present_hook.h"

#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>

#include "logger.h"
#include "camera_probe.h"
#include "camera_interceptor.h"
#include "command_list_probe.h"
#include "stereo_mode.h"
#include "backbuffer_capture.h"
#include "eye_split.h"
#include "openxr_integration.h"
#include "stall_watchdog.h"
#include "perf_stats.h"
#include "settings.h"
#include "frame_log.h"
#include "head_tracking.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

namespace vrmod {

namespace {

// Present is vtable slot 8 in IDXGISwapChain:
// 0 QueryInterface, 1 AddRef, 2 Release, 3 SetPrivateData, 4 SetPrivateDataInterface,
// 5 GetPrivateData, 6 GetParent, 7 GetDevice, 8 Present.
constexpr size_t kPresentVTableIndex = 8;

using Present_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
Present_t oPresent = nullptr;

// Present1 is slot 22 of IDXGISwapChain1 (dxgi1_2.h, SDK 10.0.26100: 18 GetDesc1, 19 GetFullscreenDesc,
// 20 GetHwnd, 21 GetCoreWindow, 22 Present1). Hooked only to count: the whole pipeline hangs off Present, so a
// game frame presented through Present1 would never reach the headset.
constexpr size_t kPresent1VTableIndex = 22;
using Present1_t = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
Present1_t oPresent1 = nullptr;
thread_local bool t_insidePresent = false;   // DXGI may implement one in terms of the other; don't count that

HRESULT __stdcall hkPresent1(IDXGISwapChain1* sc, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* params) {
    if (!t_insidePresent && !(Flags & DXGI_PRESENT_TEST)) {
        FrameLogNotePresent1();
    }
    return oPresent1(sc, SyncInterval, Flags, params);
}

unsigned long long g_frameCount = 0;
LARGE_INTEGER g_lastHeartbeatTime = {};
PhaseStat g_statFrameGap;    // time between successive Present entries (the game's frame time)
PhaseStat g_statMod;         // our work before oPresent (features + OpenXR incl. its waits)
PhaseStat g_statOPresent;    // the real Present call
double g_lastEntryMs = 0.0;

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // The game alternates a real Present (flags 0, SOTTR.exe+0xff66ad) with a
    // DXGI_PRESENT_TEST call (flags 1, SOTTR.exe+0xff6582) that presents nothing --
    // it only probes the swapchain state. Treating every call as a frame doubled
    // all our per-frame work, including xrWaitFrame, which halved the game's real
    // frame rate and made every second "frame" a repeat. Pass test calls through.
    if (Flags & DXGI_PRESENT_TEST) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    ++g_frameCount;
    const double entryMs = NowMs();

    // Per-frame recorder (Ctrl+J): opens this frame's row, with the camera struct as it stands at Present.
    {
        CamPose now;
        const bool haveNow = CameraReadStructPose(&now);
        FrameLogPresentBegin(g_frameCount, HeadTrackingGetPresentFrame(), haveNow ? &now : nullptr);
        FrameLogPollHotkeys();
    }

    // Who is calling Present, and with what arguments? (The game presents every
    // rendered frame twice in DX12 SBS; the call sites may differ between the pair.)
    if (g_frameCount >= 600 && g_frameCount < 640) {
        auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::ostringstream o;
        o << "PRESENTCALL #" << g_frameCount << " tid=" << GetCurrentThreadId() << " sync=" << SyncInterval
          << " flags=0x" << std::hex << Flags << " caller=";
        if (ret >= exe && ret < exe + 0x8000000) {
            o << "SOTTR.exe+0x" << (ret - exe);
        } else {
            o << "0x" << ret << " (outside SOTTR.exe)";
        }
        modlog::Line(o.str());
    }
    if (g_lastEntryMs > 0.0) {
        g_statFrameGap.Add(entryMs - g_lastEntryMs);
    }
    g_lastEntryMs = entryMs;
    StallWatchdogNotePresent();
    StallWatchdogSetPhase("hkPresent:features");

    ProbeStaticCandidatesIfHotkeyDown();
    LogCameraStructIfHotkeyDown();
    TestCameraWriteIfHotkeyPressed();
    PollCommandListCaptureHotkey();
    PollCameraTraceHotkey();
    StallWatchdogSetPhase("hkPresent:overlay");
    VrOverlayTick();   // before the first-person combos: while the overlay is open the game (and they) see no gamepad
    StallWatchdogSetPhase("hkPresent:features2");
    PollFirstPersonHotkeys();
    FlushCameraTraceIfReady();
    CaptureBackbufferIfHotkeyPressed(pSwapChain);
    StallWatchdogSetPhase("hkPresent:eyeSplit");
    UpdateEyeSplitTextures(pSwapChain);
    DumpEyeTexturesIfHotkeyPressed();
    StallWatchdogSetPhase("hkPresent:openxr");
    UpdateOpenXR(pSwapChain);

    if (g_frameCount == 1) {
        // The render thread sits in timed waits every frame; wake it promptly.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        modlog::Line("hkPresent: first frame intercepted. Hook is alive.");
        QueryPerformanceCounter(&g_lastHeartbeatTime);
    } else if (g_frameCount % 300 == 0) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        double seconds = static_cast<double>(now.QuadPart - g_lastHeartbeatTime.QuadPart) / static_cast<double>(freq.QuadPart);
        g_lastHeartbeatTime = now;

        std::ostringstream oss;
        oss << "hkPresent: heartbeat, frame " << g_frameCount
            << " | fps=" << std::fixed << std::setprecision(1) << (seconds > 0.0 ? 300.0 / seconds : 0.0)
            << " | frameGap " << g_statFrameGap.Summary()
            << " modWork " << g_statMod.Summary()
            << " oPresent " << g_statOPresent.Summary();
        // Process memory, to catch leaks (committed private bytes and working set of the whole game process).
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            oss << " | process private " << std::setprecision(1) << pmc.PrivateUsage / (1024.0 * 1024.0) << " MB, working set "
                << pmc.WorkingSetSize / (1024.0 * 1024.0) << " MB";
        }
        if (GetLogSwitches().perf) {
            modlog::Line(oss.str());
        }
        g_statFrameGap.Reset();
        g_statMod.Reset();
        g_statOPresent.Reset();
        if (GetLogSwitches().perf) {
            LogVideoMemory(pSwapChain, "heartbeat");
        }
    }

    // Experiment: with vrmod_offscreen.txt present, keep the (huge, borderless)
    // game window entirely off the visible desktop so DWM has nothing to
    // composite. A 7680x4320 window presented at the desktop's 60 Hz while the
    // headset runs at 72 Hz matches the log's frame-miss pattern (one late frame in
    // six = the 60/72 beat), and exclusive fullscreen, which bypasses DWM, never
    // showed it.
    static const bool offscreen = GetFileAttributesW(L"vrmod_offscreen.txt") != INVALID_FILE_ATTRIBUTES;
    if (offscreen && g_frameCount % 60 == 1) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
            HWND hwnd = nullptr;
            if (SUCCEEDED(sc1->GetHwnd(&hwnd)) && hwnd) {
                RECT r{};
                GetWindowRect(hwnd, &r);
                if (r.left > -10000) {
                    SetWindowPos(hwnd, nullptr, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    modlog::Line("hkPresent: moved the game window off-screen (vrmod_offscreen.txt).");
                }
            }
            sc1->Release();
        }
    }

    // The game asks for vsync (the in-game menu has no way to turn it off),
    // which would add a second blocking wait -- desktop refresh -- on top of
    // the headset's own frame pacing in xrWaitFrame. Pacing belongs to the XR
    // runtime alone, so always present with sync interval 0.
    StallWatchdogSetPhase("hkPresent:oPresent");
    const double beforePresentMs = NowMs();
    g_statMod.Add(beforePresentMs - entryMs);
    t_insidePresent = true;
    HRESULT hr = oPresent(pSwapChain, 0, Flags);
    t_insidePresent = false;
    g_statOPresent.Add(NowMs() - beforePresentMs);
    FrameLogPresentEnd();
    StallWatchdogSetPhase("game:between-presents");
    return hr;
}

// Creates a hidden window + throwaway D3D11 device/swapchain solely to read
// the Present function pointer out of the IDXGISwapChain vtable. The device
// and window are torn down immediately after -- the vtable lives at a fixed
// address shared by every swap chain the game creates later.
bool GetRealPresentAddress(void** outPresentAddr, void** outPresent1Addr) {
    modlog::Line("GetRealPresentAddress: waiting for the probe-device lock.");
    std::lock_guard<std::recursive_mutex> probeLock(ProbeDeviceMutex());
    modlog::Line("GetRealPresentAddress: creating throwaway device.");
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VRModDummyWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        modlog::Line("GetRealPresentAddress: failed to create dummy window.");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 100;
    scd.BufferDesc.Height = 100;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    D3D_FEATURE_LEVEL featureLevel;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swapChain, &device, &featureLevel, &context);

    if (FAILED(hr) || !swapChain) {
        modlog::Line("GetRealPresentAddress: D3D11CreateDeviceAndSwapChain failed, hr=" + std::to_string(hr));
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    void** swapChainVTable = *reinterpret_cast<void***>(swapChain);
    *outPresentAddr = swapChainVTable[kPresentVTableIndex];
    *outPresent1Addr = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        *outPresent1Addr = (*reinterpret_cast<void***>(sc1))[kPresent1VTableIndex];
        sc1->Release();
    }

    swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return true;
}

} // namespace

void LogVideoMemory(IDXGISwapChain* swapChain, const char* tag) {
    if (!swapChain) {
        return;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) || !dxgiDevice) {
        return;
    }
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
                std::ostringstream oss;
                oss << "VRAM (" << tag << "): process usage " << (local.CurrentUsage >> 20) << " MiB, budget "
                    << (local.Budget >> 20) << " MiB";
                modlog::Line(oss.str());
            }
            adapter3->Release();
        }
        adapter->Release();
    }
    dxgiDevice->Release();
}

std::recursive_mutex& ProbeDeviceMutex() {
    static std::recursive_mutex m;
    return m;
}

bool EnsureMinHookInitialized() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        ok = (MH_Initialize() == MH_OK);
        if (!ok) {
            modlog::Line("MinHook: MH_Initialize failed.");
        }
    });
    return ok;
}

bool InstallPresentHook() {
    void* presentAddr = nullptr;
    void* present1Addr = nullptr;
    if (!GetRealPresentAddress(&presentAddr, &present1Addr)) {
        return false;
    }

    if (!EnsureMinHookInitialized()) {
        modlog::Line("InstallPresentHook: MinHook init failed.");
        return false;
    }

    if (MH_CreateHook(presentAddr, &hkPresent, reinterpret_cast<void**>(&oPresent)) != MH_OK
        || MH_EnableHook(presentAddr) != MH_OK) {
        modlog::Line("InstallPresentHook: Present hook setup failed.");
        return false;
    }
    modlog::Line("InstallPresentHook: Present hook installed successfully.");

    if (present1Addr && present1Addr != presentAddr
        && MH_CreateHook(present1Addr, &hkPresent1, reinterpret_cast<void**>(&oPresent1)) == MH_OK
        && MH_EnableHook(present1Addr) == MH_OK) {
        modlog::Line("InstallPresentHook: Present1 counter installed (logs once if the game ever uses Present1).");
    } else {
        modlog::Line("InstallPresentHook: Present1 counter NOT installed (Present1 use would go unnoticed).");
    }
    return true;
}

void RemovePresentHook() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace vrmod
