#include "gpu_tier.h"

#include "logger.h"

#include "settings.h"

#include <dxgi1_2.h>
#include <winternl.h>
#include <d3dkmthk.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace vrmod {

namespace {

// Rough speed at high resolutions (4K rasterised games) against an RTX 5090, from published benchmarks. Longer names
// first, so "4070 Ti SUPER" isn't read as "4070 Ti" or "4070". Only for a starting point: the overlay changes it.
struct Tier { const char* name; float relative; };
constexpr Tier kTiers[] = {
    { "RTX 5090", 1.00f }, { "RTX 4090", 0.77f }, { "RTX 5080", 0.62f }, { "RTX 4080 SUPER", 0.58f }, { "RTX 4080", 0.56f },
    { "RTX 5070 Ti", 0.55f }, { "RTX 3090 Ti", 0.48f }, { "RTX 4070 Ti SUPER", 0.47f }, { "RTX 3090", 0.43f },
    { "RTX 4070 Ti", 0.43f }, { "RTX 5070", 0.42f }, { "RTX 3080 Ti", 0.41f }, { "RTX 4070 SUPER", 0.40f }, { "RTX 3080", 0.37f },
    { "RTX 4070", 0.36f }, { "RTX 5060 Ti", 0.31f }, { "RTX 3070 Ti", 0.29f }, { "RTX 4060 Ti", 0.27f }, { "RTX 3070", 0.27f },
    { "RTX 2080 Ti", 0.27f }, { "RTX 5060", 0.26f }, { "RTX 3060 Ti", 0.24f }, { "RTX 2080 SUPER", 0.22f }, { "RTX 4060", 0.21f },
    { "RTX 2080", 0.21f }, { "RTX 2070 SUPER", 0.19f }, { "RTX 3060", 0.18f }, { "RTX 2070", 0.17f }, { "RTX 2060 SUPER", 0.16f },
    { "RTX 2060", 0.13f }, { "RTX 3050", 0.12f },
    { "RX 7900 XTX", 0.62f }, { "RX 7900 XT", 0.52f }, { "RX 9070 XT", 0.50f }, { "RX 9070", 0.44f }, { "RX 7900 GRE", 0.42f },
    { "RX 6950 XT", 0.42f }, { "RX 6900 XT", 0.40f }, { "RX 7800 XT", 0.38f }, { "RX 6800 XT", 0.37f }, { "RX 6800", 0.31f },
    { "RX 7700 XT", 0.30f },
};
constexpr float kUnknown = 0.30f;     // a card not in the table: assume mid-range rather than the fastest
constexpr float kLaptop = 0.6f;       // "Laptop GPU": far below the desktop card of the same name

// Starting resolution model, from the development PC (RTX 5090, 72 Hz): about 10 ms of GPU work per frame at 29.2 million
// pixels, of which ~3 ms doesn't grow with the pixel count. Another card takes 1 / relative as long. Aim for 12.5 ms: a
// tenth under the 13.9 ms of a 72 Hz frame, so no automatic choice is borderline (13 ms put a 4090 at 150 %, right at it).
constexpr double kFixedMs = 3.0, kPerMpixMs = 7.0 / 29.2, kTargetMs = 12.5;

// Foveated rendering's change to the GPU time per frame at the same 29.2 million pixels (2026-10-26, RTX 5090, 150 %,
// same save and view, Cheeky's shipped settings; GPU busy off 11.04 ms): fixed 0.55 x 0.45 10.86, eye tracked 0.20 x 0.20
// 9.34 (DLSS stage 2.70 -> 1.11), eye tracked + DLSS 5 10.79. DLSS's cost grows with the pixels, so per million pixels.
// Fixed + DLSS 5 (2026-11-01, same 150 %, one spot): DLSS stage 6.75 ms against eye tracked + DLSS 5's 2.55 -- DLSS 5
// works on the whole sharp region, about six times larger when fixed -- so +4.2 on eye tracked + DLSS 5's -0.25.
constexpr double kFoveationMs[] = { 0.0, -0.18, -1.70, -0.25, 3.95 };
constexpr double kMeasuredMpix = 29.2;

std::mutex g_nameMutex;
std::string g_detectedName;

double PerMpixMs(Foveation fov) {
    return kPerMpixMs + kFoveationMs[static_cast<int>(fov)] / kMeasuredMpix;
}

// The adapter's name and video memory as Windows has them (the graphics kernel's adapter info, from the driver's own
// registry entry), not DXGI's: OptiScaler -- the route for AMD and Intel cards, Setup's option -- makes DXGI describe
// the card as an "NVIDIA GeForce RTX 4090" so the game offers DLSS, and the starting resolution would be chosen for a
// 4090. Through gdi32's D3DKMT functions, looked up at run time.
bool RealAdapter(LUID luid, std::string* name, uint64_t* vramMB) {
    const HMODULE gdi = GetModuleHandleW(L"gdi32.dll") ? GetModuleHandleW(L"gdi32.dll") : LoadLibraryW(L"gdi32.dll");
    if (!gdi) return false;
    using Open_t = NTSTATUS(APIENTRY*)(D3DKMT_OPENADAPTERFROMLUID*);
    using Query_t = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
    using Close_t = NTSTATUS(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);
    const auto open = reinterpret_cast<Open_t>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    const auto query = reinterpret_cast<Query_t>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    const auto close = reinterpret_cast<Close_t>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    if (!open || !query || !close) return false;
    D3DKMT_OPENADAPTERFROMLUID oa{};
    oa.AdapterLuid = luid;
    if (open(&oa) != 0) return false;
    D3DKMT_ADAPTERREGISTRYINFO reg{};
    D3DKMT_QUERYADAPTERINFO q{};
    q.hAdapter = oa.hAdapter;
    q.Type = KMTQAITYPE_ADAPTERREGISTRYINFO;
    q.pPrivateDriverData = &reg;
    q.PrivateDriverDataSize = sizeof(reg);
    const bool ok = query(&q) == 0 && reg.AdapterString[0];
    if (ok) {
        char n[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, reg.AdapterString, -1, n, sizeof(n) - 1, nullptr, nullptr);
        *name = n;
        D3DKMT_SEGMENTSIZEINFO seg{};
        q.Type = KMTQAITYPE_GETSEGMENTSIZE;
        q.pPrivateDriverData = &seg;
        q.PrivateDriverDataSize = sizeof(seg);
        if (query(&q) == 0 && seg.DedicatedVideoMemorySize) *vramMB = seg.DedicatedVideoMemorySize >> 20;
    }
    D3DKMT_CLOSEADAPTER ca{};
    ca.hAdapter = oa.hAdapter;
    close(&ca);
    return ok;
}

}  // namespace

void LogSystemInfo() {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    if (const HMODULE dxgi = LoadLibraryW((std::wstring(sys) + L"\\dxgi.dll").c_str())) {   // the system DXGI, not this mod
        using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
        const auto create = reinterpret_cast<CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        IDXGIFactory1* factory = nullptr;
        if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                LARGE_INTEGER umd{};
                const bool desc = SUCCEEDED(adapter->GetDesc1(&d));
                const bool ver = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
                adapter->Release();
                if (!desc || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
                char name[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                std::string real = name;
                uint64_t vram = d.DedicatedVideoMemory >> 20;
                RealAdapter(d.AdapterLuid, &real, &vram);   // Windows' own name (DXGI's may be OptiScaler's)
                std::string line = std::string("SYSTEM: graphics card '") + real + "', " + std::to_string(vram) + " MB";
                if (real != name) line += std::string(" (DXGI reports '") + name + "')";
                if (ver) {
                    const unsigned a = HIWORD(umd.HighPart), b = LOWORD(umd.HighPart), c = HIWORD(umd.LowPart), e = LOWORD(umd.LowPart);
                    line += ", driver " + std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(e);
                    if (real.find("NVIDIA") != std::string::npos) {   // NVIDIA's own numbering: the last five digits, e.g. 32.0.15.6094 -> 560.94
                        const unsigned n = (c % 10) * 10000 + e;
                        line += " (NVIDIA " + std::to_string(n / 100) + "." + (n % 100 < 10 ? "0" : "") + std::to_string(n % 100) + ")";
                    }
                }
                modlog::Line(line);
            }
            factory->Release();
        }
        FreeLibrary(dxgi);
    }
    char build[32] = {};
    DWORD size = sizeof(build);
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &size);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    ini = ini.substr(0, ini.find_last_of(L'\\') + 1) + L"vrmod_settings.ini";
    wchar_t headset[160] = {};
    GetPrivateProfileStringW(L"render", L"headset", L"", headset, 160, ini.c_str());
    char hs[320] = {};
    WideCharToMultiByte(CP_UTF8, 0, headset, -1, hs, sizeof(hs) - 1, nullptr, nullptr);
    modlog::Line(std::string("SYSTEM: Windows build ") + (build[0] ? build : "?") + ", headset of the last VR session: "
        + (hs[0] ? hs : "none yet") + ", render target " + std::to_string(static_cast<int>(SettingGet("render.target_w"))) + "x"
        + std::to_string(static_cast<int>(SettingGet("render.target_h"))) + " at " + std::to_string(static_cast<int>(SettingGet("render.resolution_pct"))) + " %");
}

GpuInfo GpuInfoForLuid(LUID luid) {
    GpuInfo g;
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    const HMODULE dxgi = LoadLibraryW((std::wstring(sys) + L"\\dxgi.dll").c_str());   // the system DXGI, not this mod
    if (!dxgi) return g;
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            const bool match = SUCCEEDED(adapter->GetDesc1(&d)) && d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart;
            adapter->Release();
            if (!match) continue;
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            g.name = name;
            g.vramMB = d.DedicatedVideoMemory >> 20;
            break;
        }
        factory->Release();
    }
    FreeLibrary(dxgi);
    {
        std::string real;
        uint64_t vram = g.vramMB;
        if (RealAdapter(luid, &real, &vram)) {
            if (!g.name.empty() && real != g.name) {
                modlog::Line("GPU: DXGI reports '" + g.name + "', Windows '" + real + "' -- the starting resolution is chosen for Windows' "
                             "(another mod, e.g. OptiScaler, changes what DXGI reports)");
            }
            g.name = real;
            g.vramMB = vram;
        }
    }
    if (g.name.empty()) return g;
    {
        std::lock_guard<std::mutex> lock(g_nameMutex);
        g_detectedName = g.name;
    }
    g.relative = kUnknown;
    for (const Tier& t : kTiers) {   // the first match: longer names come first in the table
        if (g.name.find(t.name) == std::string::npos) continue;
        g.relative = t.relative;
        g.known = true;
        break;
    }
    if (g.name.find("Laptop") != std::string::npos) g.relative *= kLaptop;
    return g;
}

std::string DetectedGpuName() {
    std::lock_guard<std::mutex> lock(g_nameMutex);
    return g_detectedName;
}

Foveation CurrentFoveation() {
    const float mode = SettingGet("foveation.mode");   // off | fixed | eye
    if (mode < 1.0f || GetFileAttributesW(L"CheekyFoveatedDLSS\\CheekyFoveatedDLSSRuntime.dll") == INVALID_FILE_ATTRIBUTES) return Foveation::Off;
    const bool dlss5 = SettingGet("foveation.dlss5") != 0.0f && GetFileAttributesW(L"nvngx_dlssnr.dll") != INVALID_FILE_ATTRIBUTES;
    if (mode < 2.0f) return dlss5 ? Foveation::FixedDlss5 : Foveation::Fixed;
    return dlss5 ? Foveation::EyeDlss5 : Foveation::Eye;
}

const char* FoveationName(Foveation f) {
    switch (f) {
    case Foveation::Fixed: return "fixed";
    case Foveation::Eye: return "eye tracked";
    case Foveation::EyeDlss5: return "eye tracked + DLSS 5";
    case Foveation::FixedDlss5: return "fixed + DLSS 5";
    default: return "off";
    }
}

void FoveaSize(Foveation f, float* width, float* height) {
    const bool fixed = f == Foveation::Fixed || f == Foveation::FixedDlss5;
    *width = fixed ? 0.55f : 0.2f;   // fixed: Cheeky's own default
    *height = fixed ? 0.45f : 0.2f;
}

int StartingResolutionPct(float relative, uint32_t targetW, uint32_t targetH, Foveation fov) {
    const double targetMpix = static_cast<double>(targetW) * targetH / 1e6;
    if (relative <= 0.0f || targetMpix <= 0.0) return 100;
    const double mpix = (kTargetMs * relative - kFixedMs) / PerMpixMs(fov);   // pixels this card renders in the frame time
    const int pct = static_cast<int>(std::floor(100.0 * mpix / targetMpix / 10.0)) * 10;
    return std::clamp(pct, 50, 150);   // 150 %: about what SteamVR's own Auto picks for the fastest cards
}

double PredictedFrameMs(float relative, uint32_t targetW, uint32_t targetH, int pct, Foveation fov) {
    if (relative <= 0.0f) return 0.0;
    const double mpix = static_cast<double>(targetW) * targetH / 1e6 * pct / 100.0;
    return (kFixedMs + PerMpixMs(fov) * mpix) / relative;
}

}  // namespace vrmod
