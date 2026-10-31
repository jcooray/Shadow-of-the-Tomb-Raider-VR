#include "overlay_pad.h"

#include <windows.h>
#include <xinput.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.foundation.collections.h>
#include <windows.gaming.input.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>

#include <MinHook.h>

#include "logger.h"
#include "present_hook.h"

namespace vrmod {

namespace {

using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

GetState_t g_ex = nullptr;             // XInputGetStateEx (ordinal 100): Guide included; never hooked
std::atomic<bool> g_block{ false };
std::atomic<uint64_t> g_gameReads{ 0 };

// One hook per XInput DLL the game may load (SOTTR tries xinput1_4, xinput1_3 and xinput9_1_0).
struct Target {
    const wchar_t* dll;
    GetState_t orig = nullptr;
    bool hooked = false;
};
Target g_targets[3] = { { L"xinput1_4.dll" }, { L"xinput1_3.dll" }, { L"xinput9_1_0.dll" } };

// What the game's own XInputGetState returned, per slot (1.0.3): Steam Input's translated pads (Steam Controller,
// DualSense, and Xbox pads while Steam Input is on) live only inside the game's XInputGetState, so XInputGetStateEx
// never saw them and the overlay had no gamepad (Homo_pixelus, 2026-10-31). Recorded before any blanking.
std::mutex g_gameMutex;
XINPUT_GAMEPAD g_gamePad[4] = {};
ULONGLONG g_gameAt[4] = {};

DWORD Filter(DWORD r, DWORD index, XINPUT_STATE* s) {
    g_gameReads.fetch_add(1, std::memory_order_relaxed);
    if (r != ERROR_SUCCESS || !s) return r;
    if (index < 4) {
        std::lock_guard<std::mutex> lock(g_gameMutex);
        g_gamePad[index] = s->Gamepad;
        g_gameAt[index] = GetTickCount64();
    }
    if (g_block.load(std::memory_order_relaxed)) {
        s->Gamepad = XINPUT_GAMEPAD{};   // connected, nothing pressed
        return r;
    }
    if ((s->Gamepad.wButtons & XINPUT_GAMEPAD_START) && g_ex) {
        XINPUT_STATE ex{};
        if (g_ex(index, &ex) == ERROR_SUCCESS && (ex.Gamepad.wButtons & kPadGuide)) s->Gamepad.wButtons &= ~XINPUT_GAMEPAD_START;
    }
    return r;
}

// The keyboard and mouse while the overlay is open (1.0.3): the game reads them through Raw Input (GetRawInputData on
// WM_INPUT), so the keys that work the overlay (arrows, Enter, Backspace, ...) also reached the game. While blocked, a
// key press becomes the release of an unused key, and mouse movement and button presses are dropped; releases still
// pass, so a key or button held as the overlay opens (Ctrl+Alt+O itself) can't stay down in the game.
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
GetRawInputData_t g_origRaw = nullptr;

UINT WINAPI HkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT headerSize) {
    const UINT r = g_origRaw(h, cmd, data, size, headerSize);
    if (cmd != RID_INPUT || !data || r == static_cast<UINT>(-1) || r < sizeof(RAWINPUTHEADER) || !g_block.load(std::memory_order_relaxed)) return r;
    RAWINPUT* ri = static_cast<RAWINPUT*>(data);
    if (ri->header.dwType == RIM_TYPEKEYBOARD && !(ri->data.keyboard.Flags & RI_KEY_BREAK)) {
        ri->data.keyboard.VKey = 0xFF;
        ri->data.keyboard.MakeCode = 0;
        ri->data.keyboard.Flags = RI_KEY_BREAK;
        ri->data.keyboard.Message = WM_KEYUP;
    } else if (ri->header.dwType == RIM_TYPEMOUSE) {
        ri->data.mouse.lLastX = ri->data.mouse.lLastY = 0;
        ri->data.mouse.usButtonFlags &= RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP
                                        | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;   // the wheel too
    }
    return r;
}

// The game's Windows.Gaming.Input gamepads (1.0.3): with Steam Input off, the game reads an Xbox One / Series pad
// through Gamepad.GetCurrentReading, not XInput, so the XInput block above never reached it -- every button pressed in
// the overlay also worked in the game (Elite Series 2 on the wireless adapter, 2026-10-31). Hooked at IGamepad's
// GetCurrentReading, vtable slot 8 (IInspectable 0-5, get_Vibration 6, put_Vibration 7, GetCurrentReading 8 -- checked
// against the 10.0.26100.0 SDK's windows.gaming.input.h). The implementation is shared by every Gamepad, so one hook
// covers them all; it needs a connected pad to find, so a worker keeps trying until one is there.
namespace wgi = ABI::Windows::Gaming::Input;
using GetReading_t = HRESULT(STDMETHODCALLTYPE*)(wgi::IGamepad*, wgi::GamepadReading*);
constexpr size_t kGetCurrentReadingIndex = 8;
GetReading_t g_origWgi = nullptr;

bool GuideHeld() {
    if (!g_ex) return false;
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE ex{};
        if (g_ex(i, &ex) == ERROR_SUCCESS && (ex.Gamepad.wButtons & kPadGuide)) return true;
    }
    return false;
}

HRESULT STDMETHODCALLTYPE HkWgiReading(wgi::IGamepad* self, wgi::GamepadReading* r) {
    const HRESULT hr = g_origWgi(self, r);
    if (FAILED(hr) || !r) return hr;
    if (g_block.load(std::memory_order_relaxed)) {
        const UINT64 t = r->Timestamp;
        *r = wgi::GamepadReading{};   // connected, nothing pressed
        r->Timestamp = t;
    } else if ((r->Buttons & wgi::GamepadButtons_Menu) && GuideHeld()) {
        r->Buttons = static_cast<wgi::GamepadButtons>(r->Buttons & ~wgi::GamepadButtons_Menu);   // Menu + Guide opens the overlay
    }
    return hr;
}

DWORD WINAPI WgiHookThread(void*) {
    const HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
    for (int attempt = 0;; ++attempt) {
        HSTRING_HEADER header{};
        HSTRING cls = nullptr;
        wgi::IGamepadStatics* statics = nullptr;
        ABI::Windows::Foundation::Collections::IVectorView<wgi::Gamepad*>* pads = nullptr;
        wgi::IGamepad* pad = nullptr;
        unsigned count = 0;
        if (SUCCEEDED(WindowsCreateStringReference(RuntimeClass_Windows_Gaming_Input_Gamepad,
                static_cast<UINT32>(wcslen(RuntimeClass_Windows_Gaming_Input_Gamepad)), &header, &cls))
            && SUCCEEDED(RoGetActivationFactory(cls, __uuidof(wgi::IGamepadStatics), reinterpret_cast<void**>(&statics)))
            && SUCCEEDED(statics->get_Gamepads(&pads)) && SUCCEEDED(pads->get_Size(&count)) && count > 0
            && SUCCEEDED(pads->GetAt(0, &pad)) && pad) {
            void* fn = (*reinterpret_cast<void***>(pad))[kGetCurrentReadingIndex];
            const bool ok = EnsureMinHookInitialized() && MH_CreateHook(fn, reinterpret_cast<void*>(&HkWgiReading),
                reinterpret_cast<void**>(&g_origWgi)) == MH_OK && MH_EnableHook(fn) == MH_OK;
            modlog::Line(ok ? "OVERLAY: hooked Windows.Gaming.Input's Gamepad reading (game input held while the overlay is open)"
                            : "OVERLAY: could not hook Windows.Gaming.Input's Gamepad reading -- the game may react to the pad in the overlay");
            pad->Release();
            pads->Release();
            statics->Release();
            break;
        }
        if (pads) pads->Release();
        if (statics) statics->Release();
        if (attempt == 0) modlog::Line("OVERLAY: no Windows.Gaming.Input gamepad yet -- its hook waits for one");
        Sleep(3000);
    }
    if (SUCCEEDED(init)) RoUninitialize();
    return 0;
}

DWORD WINAPI Hk0(DWORD i, XINPUT_STATE* s) { return Filter(g_targets[0].orig(i, s), i, s); }
DWORD WINAPI Hk1(DWORD i, XINPUT_STATE* s) { return Filter(g_targets[1].orig(i, s), i, s); }
DWORD WINAPI Hk2(DWORD i, XINPUT_STATE* s) { return Filter(g_targets[2].orig(i, s), i, s); }
GetState_t const kDetours[3] = { &Hk0, &Hk1, &Hk2 };

}  // namespace

void OverlayPadPoll() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    if (!g_ex) {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput1_3.dll");
        if (m) g_ex = reinterpret_cast<GetState_t>(GetProcAddress(m, reinterpret_cast<LPCSTR>(100)));
        modlog::Line(g_ex ? "OVERLAY: gamepad read through XInputGetStateEx (Guide button available)"
                          : "OVERLAY: XInputGetStateEx not found -- Menu + Guide can't open the overlay");
    }
    static bool wgiStarted = false;
    if (!wgiStarted) {
        wgiStarted = true;
        if (const HANDLE t = CreateThread(nullptr, 0, WgiHookThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    static bool rawTried = false;
    if (!rawTried) {
        rawTried = true;
        const bool ok = EnsureMinHookInitialized()
            && MH_CreateHookApi(L"user32", "GetRawInputData", reinterpret_cast<void*>(&HkGetRawInputData), reinterpret_cast<void**>(&g_origRaw)) == MH_OK
            && MH_EnableHook(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetRawInputData"))) == MH_OK;
        modlog::Line(ok ? "OVERLAY: hooked GetRawInputData (the game's keyboard and mouse held while the overlay is open)"
                        : "OVERLAY: could not hook GetRawInputData -- keys used in the overlay also reach the game");
    }
    for (int t = 0; t < 3; ++t) {
        Target& tg = g_targets[t];
        if (tg.hooked) continue;
        HMODULE m = GetModuleHandleW(tg.dll);
        if (!m) continue;
        tg.hooked = true;   // one attempt per DLL
        void* fn = reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState"));
        if (!fn || !EnsureMinHookInitialized()
            || MH_CreateHook(fn, reinterpret_cast<void*>(kDetours[t]), reinterpret_cast<void**>(&tg.orig)) != MH_OK
            || MH_EnableHook(fn) != MH_OK) {
            modlog::Line(std::string("OVERLAY: could not hook XInputGetState in ") + (t == 0 ? "xinput1_4" : t == 1 ? "xinput1_3" : "xinput9_1_0")
                         + " -- the game may react to the gamepad while the overlay is open");
            continue;
        }
        modlog::Line(std::string("OVERLAY: hooked XInputGetState in ") + (t == 0 ? "xinput1_4" : t == 1 ? "xinput1_3" : "xinput9_1_0")
                     + ".dll (game input held while the overlay is open)");
    }
}

bool OverlayPadRead(PadState* out) {
    *out = PadState{};
    const ULONGLONG now = GetTickCount64();
    const auto merge = [out](const XINPUT_GAMEPAD& g) {
        out->connected = true;
        out->buttons |= g.wButtons;
        if (std::abs(g.sThumbLX) > std::abs(out->lx)) out->lx = g.sThumbLX;
        if (std::abs(g.sThumbLY) > std::abs(out->ly)) out->ly = g.sThumbLY;
        if (g.bLeftTrigger > out->lt) out->lt = g.bLeftTrigger;
        if (g.bRightTrigger > out->rt) out->rt = g.bRightTrigger;
    };
    {   // the pads the game reads itself (Steam Input's included), while it keeps reading them
        std::lock_guard<std::mutex> lock(g_gameMutex);
        for (int i = 0; i < 4; ++i) if (g_gameAt[i] && now - g_gameAt[i] < 500) merge(g_gamePad[i]);
    }
    if (!g_ex) return out->connected;
    // Disconnected slots are slow to query, so the set is rescanned every 2 s (as camera_interceptor's PadCombos).
    static unsigned connected = 0;
    static ULONGLONG lastScan = 0;
    if (now - lastScan >= 2000) {
        lastScan = now;
        unsigned found = 0;
        XINPUT_STATE probe{};
        for (DWORD i = 0; i < 4; ++i) if (g_ex(i, &probe) == ERROR_SUCCESS) found |= 1u << i;
        connected = found;
    }
    for (DWORD i = 0; i < 4; ++i) {
        if (!(connected & (1u << i))) continue;
        XINPUT_STATE st{};
        if (g_ex(i, &st) != ERROR_SUCCESS) { connected &= ~(1u << i); continue; }
        merge(st.Gamepad);
    }
    return out->connected;
}

void OverlayPadBlockGame(bool block) {
    g_block.store(block, std::memory_order_relaxed);
}

uint64_t OverlayPadGameReads() {
    return g_gameReads.load(std::memory_order_relaxed);
}

}  // namespace vrmod
