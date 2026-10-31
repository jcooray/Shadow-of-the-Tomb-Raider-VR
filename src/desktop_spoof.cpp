#include "desktop_spoof.h"

#include <dxgi.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>

#include "logger.h"
#include "present_hook.h"
#include "settings.h"

namespace vrmod {

namespace {

constexpr size_t kOutputGetDescIndex = 7;   // IDXGIOutput::GetDesc (IUnknown 0-2, IDXGIObject 3-6)

bool g_active = false;
UINT g_fakeW = 7680;
UINT g_fakeH = 4320;

// Self-healing render resolution (1.0.1, 2026-10-31). 1.0.0 players whose game closed itself at start-up at the render
// resolution the mod chose (before creating its window: alvaro, 5048x5240 on a 3840x2160 monitor) were stuck until they
// deleted vrmod_settings.ini. Every start that applies a render resolution leaves this file until the game's window swap
// chain exists; finding it at the next start means that start never got there, so this one steps down -- to 100 %, then
// to the game's own resolution -- and the overlay says so.
constexpr const char* kPendingFile = "vrmod_resolution_pending.txt";
std::atomic<bool> g_pending{false};
int g_fallbackFrom = 0;
int g_fallbackTo = -1;   // -1: no fallback this start; 0: the game's own resolution

using EnumDisplaySettingsW_t = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using EnumDisplaySettingsExW_t = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*, DWORD);
using GetMonitorInfoW_t = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using GetSystemMetrics_t = int(WINAPI*)(int);
using OutputGetDesc_t = HRESULT(__stdcall*)(IDXGIOutput*, DXGI_OUTPUT_DESC*);

EnumDisplaySettingsW_t g_origEnumW = nullptr;
EnumDisplaySettingsExW_t g_origEnumExW = nullptr;
GetMonitorInfoW_t g_origGetMonitorInfoW = nullptr;
GetSystemMetrics_t g_origGetSystemMetrics = nullptr;
OutputGetDesc_t g_origOutputGetDesc = nullptr;

std::atomic<int> g_logBudget{40};

void LogHit(const char* what, const std::string& detail) {
    if (g_logBudget.fetch_sub(1) > 0) {
        modlog::Line(std::string("DesktopSpoof: ") + what + " " + detail);
    }
}

BOOL WINAPI hkEnumDisplaySettingsW(LPCWSTR dev, DWORD mode, DEVMODEW* dm) {
    BOOL r = g_origEnumW(dev, mode, dm);
    if (r && dm && mode == ENUM_CURRENT_SETTINGS) {
        LogHit("EnumDisplaySettingsW(current)", std::to_string(dm->dmPelsWidth) + "x" + std::to_string(dm->dmPelsHeight)
            + " -> " + std::to_string(g_fakeW) + "x" + std::to_string(g_fakeH));
        dm->dmPelsWidth = g_fakeW;
        dm->dmPelsHeight = g_fakeH;
    }
    return r;
}

BOOL WINAPI hkEnumDisplaySettingsExW(LPCWSTR dev, DWORD mode, DEVMODEW* dm, DWORD flags) {
    BOOL r = g_origEnumExW(dev, mode, dm, flags);
    if (r && dm && mode == ENUM_CURRENT_SETTINGS) {
        LogHit("EnumDisplaySettingsExW(current)", std::to_string(dm->dmPelsWidth) + "x" + std::to_string(dm->dmPelsHeight)
            + " -> " + std::to_string(g_fakeW) + "x" + std::to_string(g_fakeH));
        dm->dmPelsWidth = g_fakeW;
        dm->dmPelsHeight = g_fakeH;
    }
    return r;
}

BOOL WINAPI hkGetMonitorInfoW(HMONITOR mon, LPMONITORINFO mi) {
    BOOL r = g_origGetMonitorInfoW(mon, mi);
    if (r && mi && (mi->dwFlags & MONITORINFOF_PRIMARY)) {
        LogHit("GetMonitorInfoW(primary)", std::to_string(mi->rcMonitor.right - mi->rcMonitor.left) + "x"
            + std::to_string(mi->rcMonitor.bottom - mi->rcMonitor.top) + " -> " + std::to_string(g_fakeW) + "x" + std::to_string(g_fakeH));
        mi->rcMonitor = { 0, 0, static_cast<LONG>(g_fakeW), static_cast<LONG>(g_fakeH) };
        mi->rcWork = mi->rcMonitor;
    }
    return r;
}

int WINAPI hkGetSystemMetrics(int index) {
    int v = g_origGetSystemMetrics(index);
    if (index == SM_CXSCREEN) { LogHit("GetSystemMetrics(CXSCREEN)", std::to_string(v)); return static_cast<int>(g_fakeW); }
    if (index == SM_CYSCREEN) { LogHit("GetSystemMetrics(CYSCREEN)", std::to_string(v)); return static_cast<int>(g_fakeH); }
    return v;
}

// The game derives its "Monitor Aspect Ratio" (which the log prints, and which feeds its projection) from the
// physical size in millimetres GetDeviceCaps reports for the monitor's DC: aspect = HORZSIZE / VERTSIZE, pixel aspect
// = (VERTSIZE * HORZRES) / (VERTRES * HORZSIZE). On a 16:9 TV that is always ~1.775, whatever resolution is chosen,
// so a non-16:9 SBS size still gets a 16:9 projection. Report the fake desktop's size at 0.25 mm per pixel instead.
using GetDeviceCaps_t = int(WINAPI*)(HDC, int);
GetDeviceCaps_t g_origGetDeviceCaps = nullptr;
UINT g_realW = 0, g_realH = 0;
bool g_aspectOverridden = false;

int WINAPI hkGetDeviceCaps(HDC hdc, int index) {
    int v = g_origGetDeviceCaps(hdc, index);
    if (index == HORZSIZE || index == VERTSIZE || index == HORZRES || index == VERTRES) {
        // Only the real desktop's DC: memory DCs, printers etc. must keep their own values.
        if (static_cast<UINT>(g_origGetDeviceCaps(hdc, HORZRES)) == g_realW && static_cast<UINT>(g_origGetDeviceCaps(hdc, VERTRES)) == g_realH) {
            int fake = index == HORZSIZE ? static_cast<int>(g_fakeW / 4) : index == VERTSIZE ? static_cast<int>(g_fakeH / 4)
                     : index == HORZRES ? static_cast<int>(g_fakeW) : static_cast<int>(g_fakeH);
            LogHit("GetDeviceCaps", std::to_string(index) + ": " + std::to_string(v) + " -> " + std::to_string(fake));
            return fake;
        }
    }
    return v;
}

HRESULT __stdcall hkOutputGetDesc(IDXGIOutput* self, DXGI_OUTPUT_DESC* desc) {
    HRESULT hr = g_origOutputGetDesc(self, desc);
    if (SUCCEEDED(hr) && desc) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (g_origGetMonitorInfoW(desc->Monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
            LogHit("IDXGIOutput::GetDesc", std::to_string(desc->DesktopCoordinates.right - desc->DesktopCoordinates.left) + "x"
                + std::to_string(desc->DesktopCoordinates.bottom - desc->DesktopCoordinates.top) + " -> "
                + std::to_string(g_fakeW) + "x" + std::to_string(g_fakeH));
            desc->DesktopCoordinates = { 0, 0, static_cast<LONG>(g_fakeW), static_cast<LONG>(g_fakeH) };
        }
    }
    return hr;
}

// The game's own error boxes, logged (1.0.1): a game that closes itself at start-up usually says why first, and players'
// logs then show it.
using MessageBoxW_t = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
using MessageBoxA_t = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
MessageBoxW_t g_origMessageBoxW = nullptr;
MessageBoxA_t g_origMessageBoxA = nullptr;

std::string Utf8(LPCWSTR s) {
    if (!s) return {};
    char buf[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return buf;
}

int WINAPI hkMessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type) {
    modlog::Line("GAME MESSAGE: \"" + Utf8(caption) + "\": " + Utf8(text));
    return g_origMessageBoxW(hwnd, text, caption, type);
}

int WINAPI hkMessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) {
    modlog::Line(std::string("GAME MESSAGE: \"") + (caption ? caption : "") + "\": " + (text ? text : ""));
    return g_origMessageBoxA(hwnd, text, caption, type);
}

void LogGameMessages() {
    if (!EnsureMinHookInitialized()) return;
    for (auto [proc, detour, original] : { std::tuple{ "MessageBoxW", reinterpret_cast<void*>(&hkMessageBoxW), reinterpret_cast<void**>(&g_origMessageBoxW) },
                                            std::tuple{ "MessageBoxA", reinterpret_cast<void*>(&hkMessageBoxA), reinterpret_cast<void**>(&g_origMessageBoxA) } }) {
        void* target = nullptr;
        if (MH_CreateHookApiEx(L"user32", proc, detour, original, &target) == MH_OK) MH_EnableHook(target);
    }
}

bool HookApi(const wchar_t* module, const char* proc, void* detour, void** original) {
    if (MH_CreateHookApiEx(module, proc, detour, original, nullptr) != MH_OK) {
        modlog::Line(std::string("DesktopSpoof: failed to hook ") + proc);
        return false;
    }
    return true;
}

} // namespace

void RealDesktopMode(UINT* w, UINT* h) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW_t fn = g_origEnumW ? g_origEnumW : &EnumDisplaySettingsW;
    if (fn(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight) {
        *w = dm.dmPelsWidth;
        *h = dm.dmPelsHeight;
    } else {
        *w = 3840;
        *h = 2160;
    }
}

// [render] resolution_pct (in-headset overlay, "Render resolution"): a percentage of the PIXELS of the headset's target
// render size (render.target_w / target_h, recorded by the OpenXR side) -- SteamVR's convention: 150 % = 1.22 x each side. Applied here, before the game reads its settings: the fake
// desktop gets that size, and the game's saved fullscreen resolution is set to it (so the game starts at it).
bool ResolutionFromSettings(unsigned* w, unsigned* h) {
    float pct = SettingGet("render.resolution_pct");
    const float tw = SettingGet("render.target_w"), th = SettingGet("render.target_h");
    if (pct < 30.0f || tw < 640.0f || th < 480.0f) return false;
    const wchar_t* key = L"Software\\Eidos Montreal\\Shadow of the Tomb Raider\\Graphics";
    FILE* f = nullptr;
    if (fopen_s(&f, kPendingFile, "r") == 0 && f) {   // the last start at a render resolution never reached the game's window
        int last = 0;
        if (fscanf_s(f, "%d", &last) != 1 || last < 30) last = static_cast<int>(pct);
        fclose(f);
        remove(kPendingFile);
        g_fallbackFrom = last;
        g_fallbackTo = last > 100 ? 100 : 0;
        SettingSet("render.resolution_pct", static_cast<float>(g_fallbackTo));
        SettingSet("render.resolution_auto", 0.0f);   // not chosen again for the graphics card
        SettingsWriteKeys({ { "render.resolution_pct", std::to_string(g_fallbackTo) }, { "render.resolution_auto", "0" } });
        modlog::Line("DesktopSpoof: the last start at render resolution " + std::to_string(last) + " % never reached the game's window "
            "(the game closed itself?) -- " + (g_fallbackTo ? std::string("100 % this time") : std::string("the game's own resolution this time")));
        if (!g_fallbackTo) {   // the game's fullscreen resolution back to the monitor's, which it starts at by itself
            UINT rw = 0, rh = 0;
            RealDesktopMode(&rw, &rh);
            const DWORD dw = rw, dh = rh;
            RegSetKeyValueW(HKEY_CURRENT_USER, key, L"FullscreenWidth", REG_DWORD, &dw, sizeof(dw));
            RegSetKeyValueW(HKEY_CURRENT_USER, key, L"FullscreenHeight", REG_DWORD, &dh, sizeof(dh));
            return false;
        }
        pct = static_cast<float>(g_fallbackTo);
    }
    const float k = std::sqrt(pct / 100.0f);
    *w = static_cast<unsigned>(tw * k / 8.0f + 0.5f) * 8;
    *h = static_cast<unsigned>(th * k / 8.0f + 0.5f) * 8;
    if (*w < 640 || *h < 480 || *w > 16384 || *h > 16384) return false;
    if (fopen_s(&f, kPendingFile, "w") == 0 && f) {   // removed once the game's window swap chain exists
        fprintf(f, "%d %ux%u\n", static_cast<int>(pct), *w, *h);
        fclose(f);
        g_pending.store(true);
    }
    const DWORD dw = *w, dh = *h, one = 1;
    const bool ok = RegSetKeyValueW(HKEY_CURRENT_USER, key, L"FullscreenWidth", REG_DWORD, &dw, sizeof(dw)) == ERROR_SUCCESS
        && RegSetKeyValueW(HKEY_CURRENT_USER, key, L"FullscreenHeight", REG_DWORD, &dh, sizeof(dh)) == ERROR_SUCCESS
        && RegSetKeyValueW(HKEY_CURRENT_USER, key, L"Fullscreen", REG_DWORD, &one, sizeof(one)) == ERROR_SUCCESS;
    modlog::Line("DesktopSpoof: render resolution " + std::to_string(static_cast<int>(pct)) + " % of the headset's target "
        + std::to_string(static_cast<int>(tw)) + "x" + std::to_string(static_cast<int>(th)) + " -> " + std::to_string(*w) + "x" + std::to_string(*h)
        + (ok ? " (game's fullscreen resolution set to it)" : " (could NOT set the game's resolution in the registry)"));
    return true;
}

// The game's Stereoscopic 3D option must be off for alternate-eye (and mono) rendering -- the mod renders one view per
// frame -- and on for side by side. Set before the game reads its settings: off for aer / mono; for sbs the log asks
// for it (the value the game uses for "on" isn't known here).
void ApplyGameStereoSetting() {
    const wchar_t* key = L"Software\\Eidos Montreal\\Shadow of the Tomb Raider\\Graphics";
    DWORD v = 0, size = sizeof(v);
    const bool have = RegGetValueW(HKEY_CURRENT_USER, key, L"Stereoscopic3DMode", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS;
    if (GetRenderMode() != RenderMode::Sbs) {
        if (have && v != 0) {
            const DWORD off = 0;
            const bool ok = RegSetKeyValueW(HKEY_CURRENT_USER, key, L"Stereoscopic3DMode", REG_DWORD, &off, sizeof(off)) == ERROR_SUCCESS;
            modlog::Line(std::string("DesktopSpoof: the game's Stereoscopic 3D was on (") + std::to_string(v) + ") -- "
                         + (ok ? "switched off for alternate-eye rendering." : "could NOT switch it off: turn it off in the game's display settings."));
        }
    } else if (have && v == 0) {
        modlog::Line("DesktopSpoof: render mode sbs needs the game's Stereoscopic 3D on -- turn it on in the game's display settings.");
    }
}

bool InstallDesktopSpoof() {
    ApplyGameStereoSetting();
    LogGameMessages();
    unsigned w = 0, h = 0;
    if (ResolutionFromSettings(&w, &h)) {
        g_fakeW = w;
        g_fakeH = h;
    } else if (g_fallbackTo == 0) {
        return true;   // the self-healing last step: the game's own resolution, no fake desktop (not even vrmod_fakedesktop.txt's)
    } else {
        FILE* f = nullptr;
        if (fopen_s(&f, "vrmod_fakedesktop.txt", "r") != 0 || !f) {
            return true;   // feature off, nothing installed
        }
        if (fscanf_s(f, "%u %u", &w, &h) == 2 && w >= 640 && h >= 480 && w <= 16384 && h <= 16384) {
            g_fakeW = w;
            g_fakeH = h;
        }
        fclose(f);
    }

    if (!EnsureMinHookInitialized()) {
        return false;
    }
    RealDesktopMode(&g_realW, &g_realH);   // before the enumeration hooks exist
    bool ok = true;
    ok &= HookApi(L"user32", "EnumDisplaySettingsW", reinterpret_cast<void*>(&hkEnumDisplaySettingsW), reinterpret_cast<void**>(&g_origEnumW));
    ok &= HookApi(L"user32", "EnumDisplaySettingsExW", reinterpret_cast<void*>(&hkEnumDisplaySettingsExW), reinterpret_cast<void**>(&g_origEnumExW));
    ok &= HookApi(L"user32", "GetMonitorInfoW", reinterpret_cast<void*>(&hkGetMonitorInfoW), reinterpret_cast<void**>(&g_origGetMonitorInfoW));
    ok &= HookApi(L"user32", "GetSystemMetrics", reinterpret_cast<void*>(&hkGetSystemMetrics), reinterpret_cast<void**>(&g_origGetSystemMetrics));
    // Only worth spoofing when the fake desktop's shape differs from 16:9; otherwise leave GDI alone.
    const double fakeAspect = static_cast<double>(g_fakeW) / static_cast<double>(g_fakeH);
    if (fakeAspect < 1.74 || fakeAspect > 1.82) {
        g_aspectOverridden = HookApi(L"gdi32", "GetDeviceCaps", reinterpret_cast<void*>(&hkGetDeviceCaps), reinterpret_cast<void**>(&g_origGetDeviceCaps));
        if (g_aspectOverridden) {
            modlog::Line("DesktopSpoof: GetDeviceCaps spoofed too (monitor aspect follows the fake desktop: " + std::to_string(fakeAspect) + ").");
        }
    }
    if (ok) {
        MH_EnableHook(MH_ALL_HOOKS);
        g_active = true;
        modlog::Line("DesktopSpoof: ACTIVE, the game will see a " + std::to_string(g_fakeW) + "x" + std::to_string(g_fakeH) + " desktop.");
    }
    return ok;
}

bool DesktopSpoofSize(unsigned* w, unsigned* h) {
    *w = g_fakeW;
    *h = g_fakeH;
    return g_active;
}

void DesktopSpoofGameWindowCreated(unsigned w, unsigned h) {
    if (!g_pending.exchange(false)) return;
    remove(kPendingFile);
    modlog::Line("DesktopSpoof: the game's window swap chain exists (" + std::to_string(w) + "x" + std::to_string(h)
        + ") -- it starts fine at this render resolution.");
}

bool DesktopSpoofFallback(int* fromPct, int* toPct) {
    *fromPct = g_fallbackFrom;
    *toPct = g_fallbackTo;
    return g_fallbackTo >= 0;
}

bool DesktopSpoofAspectOverridden() {
    return g_aspectOverridden;
}

void DesktopSpoofHookOutput(IDXGIOutput* output) {
    if (!g_active || !output) {
        return;
    }
    void** vt = *reinterpret_cast<void***>(output);
    void* target = vt[kOutputGetDescIndex];
    if (MH_CreateHook(target, &hkOutputGetDesc, reinterpret_cast<void**>(&g_origOutputGetDesc)) == MH_OK
        && MH_EnableHook(target) == MH_OK) {
        modlog::Line("DesktopSpoof: hooked IDXGIOutput::GetDesc.");
    } else {
        modlog::Line("DesktopSpoof: failed to hook IDXGIOutput::GetDesc.");
    }
}

} // namespace vrmod
