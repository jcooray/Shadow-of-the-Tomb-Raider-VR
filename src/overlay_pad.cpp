#include "overlay_pad.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cstdlib>
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

DWORD Filter(DWORD r, DWORD index, XINPUT_STATE* s) {
    g_gameReads.fetch_add(1, std::memory_order_relaxed);
    if (r != ERROR_SUCCESS || !s) return r;
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
    if (!g_ex) return false;
    // Disconnected slots are slow to query, so the set is rescanned every 2 s (as camera_interceptor's PadCombos).
    static unsigned connected = 0;
    static ULONGLONG lastScan = 0;
    const ULONGLONG now = GetTickCount64();
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
        out->connected = true;
        out->buttons |= st.Gamepad.wButtons;
        if (std::abs(st.Gamepad.sThumbLX) > std::abs(out->lx)) out->lx = st.Gamepad.sThumbLX;
        if (std::abs(st.Gamepad.sThumbLY) > std::abs(out->ly)) out->ly = st.Gamepad.sThumbLY;
        if (st.Gamepad.bLeftTrigger > out->lt) out->lt = st.Gamepad.bLeftTrigger;
        if (st.Gamepad.bRightTrigger > out->rt) out->rt = st.Gamepad.bRightTrigger;
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
