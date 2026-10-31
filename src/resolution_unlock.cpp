#include "resolution_unlock.h"
#include "audio_cue.h"
#include "frame_log.h"

#include <windows.h>
#include <intrin.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "logger.h"
#include "present_hook.h"
#include "desktop_spoof.h"
#include "openxr_integration.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace vrmod {

namespace {

// Verified against the Windows 10.0.26100.0 SDK's dxgi.h / dxgi1_2.h macro
// order: IDXGIOutput has 19 slots (0-18), GetDesc=7, GetDisplayModeList=8,
// FindClosestMatchingMode=9; IDXGIOutput1 continues at GetDisplayModeList1=19,
// FindClosestMatchingMode1=20.
constexpr size_t kGetDisplayModeListIndex = 8;
constexpr size_t kFindClosestMatchingModeIndex = 9;
constexpr size_t kGetDisplayModeList1Index = 19;
constexpr size_t kFindClosestMatchingMode1Index = 20;

struct ExtraMode {
    UINT width;
    UINT height;
};

const ExtraMode kExtraModes[] = {
    // 16:9
    { 5120, 2880 }, { 5760, 3240 }, { 6720, 3780 }, { 7680, 4320 },
    // ~1.125:1 -- matches the Dream Air's per-eye field of view (about 82 x 76 deg, tan span 1.76 x 1.57), so no
    // pixels are wasted outside what the panel can show. Per eye these are 2160x3840, 2700x4800, 3024x5376.
    { 4320, 3840 }, { 5400, 4800 }, { 6048, 5376 },
    // 2:1 -- exactly 3840x3840 per eye, the Dream Air's panel size
    { 7680, 3840 },
    // The Dream Air's actual SteamVR "100%" per-eye size (5132x3984), confirmed against the sboys3 driver's
    // published native figure via SteamVR's own 1.5x auto supersample multiplier. For a look, not for real use --
    // costs ~23% more than the 8K (3840x4320/eye) config already measured.
    { 10264, 3984 },
    // Above 8K (DX12 fullscreen window with vrmod_fakedesktop.txt set to the same size)
    { 9600, 5400 }, { 11520, 6480 },
    // 4:3
    { 4800, 3600 }, { 5760, 4320 },
    // 5:4
    { 4500, 3600 }, { 5400, 4320 },
};

// The fixed list plus the size in vrmod_fakedesktop.txt (if any), so whatever size the fake desktop is set to --
// e.g. the Phase 6 render size computed from the headset's view -- can be picked in the game's menu.
const std::vector<ExtraMode>& AllExtraModes() {
    static const std::vector<ExtraMode> all = [] {
        std::vector<ExtraMode> v(std::begin(kExtraModes), std::end(kExtraModes));
        // The size the fake desktop really has (the overlay's Render resolution, set up before the game enumerates
        // modes) -- 2026-10-21: only the file's size was offered, so the game couldn't pick 6336x4592 and took the file's
        // 6368x4592 instead (stretched to the 6336 swapchain).
        unsigned sw = 0, sh = 0;
        if (DesktopSpoofSize(&sw, &sh) && sw >= 640 && sh >= 480) {
            bool present = false;
            for (const auto& m : v) if (m.width == sw && m.height == sh) present = true;
            if (!present) v.push_back({ sw, sh });
        }
        FILE* fp = nullptr;
        UINT w = 0, h = 0;
        if (fopen_s(&fp, "vrmod_fakedesktop.txt", "r") == 0 && fp) {
            if (fscanf_s(fp, "%u %u", &w, &h) == 2 && w >= 640 && h >= 480 && w <= 16384 && h <= 16384) {
                bool present = false;
                for (const auto& m : v) if (m.width == w && m.height == h) present = true;
                if (!present) v.push_back({ w, h });
            }
            fclose(fp);
        }
        return v;
    }();
    return all;
}

bool IsExtraMode(UINT w, UINT h) {
    for (const auto& m : AllExtraModes()) {
        if (m.width == w && m.height == h) {
            return true;
        }
    }
    return false;
}

template <typename Desc>
void AppendExtraModes(std::vector<Desc>* modes, UINT* outMaxW, UINT* outMaxH, size_t* outAdded) {
    unsigned long long maxArea = 0;
    UINT maxW = 0, maxH = 0;
    for (const auto& d : *modes) {
        unsigned long long area = static_cast<unsigned long long>(d.Width) * d.Height;
        if (area > maxArea) {
            maxArea = area;
            maxW = d.Width;
            maxH = d.Height;
        }
    }
    *outMaxW = maxW;
    *outMaxH = maxH;
    *outAdded = 0;
    if (maxArea == 0) {
        return;
    }

    // Clone every refresh-rate variant of the largest real mode for each extra
    // resolution, so format/scaling/scanline fields are ones the game has
    // already seen and accepted.
    std::vector<Desc> templates;
    for (const auto& d : *modes) {
        if (d.Width == maxW && d.Height == maxH) {
            templates.push_back(d);
        }
    }
    for (const auto& extra : AllExtraModes()) {
        bool alreadyPresent = false;
        for (const auto& d : *modes) {
            if (d.Width == extra.width && d.Height == extra.height) {
                alreadyPresent = true;
                break;
            }
        }
        if (alreadyPresent) {
            continue;
        }
        for (Desc t : templates) {
            t.Width = extra.width;
            t.Height = extra.height;
            modes->push_back(t);
            ++*outAdded;
        }
    }
}

// Module-relative address of the game code that called into us, so the code
// that consumes (and filters) the mode list can be found in Ghidra.
// Called from SOTTR.exe itself (not the mod's own or an overlay's throwaway swap chains).
bool CalledFromGame(void* returnAddress) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto ra = reinterpret_cast<uintptr_t>(returnAddress);
    return ra >= base && ra < base + 0x10000000;
}

std::string CallerRva(void* returnAddress) {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto ra = reinterpret_cast<uintptr_t>(returnAddress);
    std::ostringstream oss;
    if (ra >= base && ra < base + 0x10000000) {
        oss << "SOTTR.exe+0x" << std::hex << (ra - base);
    } else {
        oss << "0x" << std::hex << ra << " (outside SOTTR.exe)";
    }
    return oss.str();
}

template <typename Desc>
std::string UniqueResolutions(const std::vector<Desc>& modes) {
    std::vector<std::pair<UINT, UINT>> u;
    for (const auto& d : modes) {
        auto p = std::make_pair(d.Width, d.Height);
        if (std::find(u.begin(), u.end(), p) == u.end()) {
            u.push_back(p);
        }
    }
    std::sort(u.begin(), u.end());
    std::ostringstream oss;
    for (const auto& p : u) {
        oss << p.first << "x" << p.second << " ";
    }
    return oss.str();
}

template <typename Desc, typename OrigFn, typename Self>
HRESULT ExtendModeList(OrigFn orig, Self* self, DXGI_FORMAT format, UINT flags, UINT* pNumModes, Desc* pDesc, const char* tag, void* returnAddress) {
    if (!pNumModes || !orig) {
        return orig ? orig(self, format, flags, pNumModes, pDesc) : E_POINTER;
    }

    UINT realCount = 0;
    HRESULT hr = orig(self, format, flags, &realCount, nullptr);
    if (FAILED(hr)) {
        return orig(self, format, flags, pNumModes, pDesc);
    }
    std::vector<Desc> modes(realCount);
    if (realCount > 0) {
        hr = orig(self, format, flags, &realCount, modes.data());
        if (FAILED(hr) && hr != DXGI_ERROR_MORE_DATA) {
            return orig(self, format, flags, pNumModes, pDesc);
        }
        modes.resize(realCount);
    }

    static std::atomic<int> logged{0};
    bool logThis = logged.fetch_add(1) < 6;
    std::string realResolutions;
    if (logThis) {
        realResolutions = UniqueResolutions(modes);
    }

    UINT maxW = 0, maxH = 0;
    size_t added = 0;
    AppendExtraModes(&modes, &maxW, &maxH, &added);

    if (logThis) {
        std::ostringstream oss;
        oss << "ResolutionUnlock: " << tag << " format=" << format << " flags=" << flags
            << " capacityIn=" << *pNumModes << " real modes=" << realCount
            << " largest=" << maxW << "x" << maxH << " injected=" << added
            << (pDesc ? " (fill call)" : " (count call)")
            << " caller=" << CallerRva(returnAddress);
        modlog::Line(oss.str());
        if (!pDesc) {
            modlog::Line("ResolutionUnlock: real unique resolutions: " + realResolutions);
        }
    }

    if (!pDesc) {
        *pNumModes = static_cast<UINT>(modes.size());
        return S_OK;
    }
    UINT copy = std::min<UINT>(*pNumModes, static_cast<UINT>(modes.size()));
    std::memcpy(pDesc, modes.data(), copy * sizeof(Desc));
    *pNumModes = copy;
    return copy < modes.size() ? DXGI_ERROR_MORE_DATA : S_OK;
}

template <typename Desc, typename OrigFn, typename Self>
HRESULT FindClosestInjected(OrigFn orig, Self* self, const Desc* match, Desc* closest, IUnknown* device, const char* tag, void* returnAddress) {
    static std::atomic<int> calls{0};
    if (match && calls.fetch_add(1) < 8) {
        modlog::Line(std::string("ResolutionUnlock: ") + tag + " called for "
            + std::to_string(match->Width) + "x" + std::to_string(match->Height)
            + " caller=" + CallerRva(returnAddress));
    }
    if (match && closest && IsExtraMode(match->Width, match->Height)) {
        *closest = *match;
        if (closest->RefreshRate.Denominator == 0) {
            closest->RefreshRate.Numerator = 60;
            closest->RefreshRate.Denominator = 1;
        }
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 6) {
            modlog::Line(std::string("ResolutionUnlock: ") + tag + " accepted injected mode "
                + std::to_string(match->Width) + "x" + std::to_string(match->Height));
        }
        return S_OK;
    }
    return orig(self, match, closest, device);
}

using GetDisplayModeList_t = HRESULT(__stdcall*)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*);
using FindClosest_t = HRESULT(__stdcall*)(IDXGIOutput*, const DXGI_MODE_DESC*, DXGI_MODE_DESC*, IUnknown*);
using GetDisplayModeList1_t = HRESULT(__stdcall*)(IDXGIOutput1*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC1*);
using FindClosest1_t = HRESULT(__stdcall*)(IDXGIOutput1*, const DXGI_MODE_DESC1*, DXGI_MODE_DESC1*, IUnknown*);

// IDXGIOutput and IDXGIOutput1 may or may not share function pointers for the
// inherited slots, so the IDXGIOutput-signature hooks are instantiated twice
// (Id 0 for the IDXGIOutput vtable, Id 1 for the IDXGIOutput1 vtable) with
// separate trampolines, and only installed if the targets differ.
template <int Id> GetDisplayModeList_t g_origGetDisplayModeList = nullptr;
template <int Id> FindClosest_t g_origFindClosest = nullptr;
GetDisplayModeList1_t g_origGetDisplayModeList1 = nullptr;
FindClosest1_t g_origFindClosest1 = nullptr;

template <int Id>
HRESULT __stdcall hkGetDisplayModeList(IDXGIOutput* self, DXGI_FORMAT format, UINT flags, UINT* pNumModes, DXGI_MODE_DESC* pDesc) {
    return ExtendModeList<DXGI_MODE_DESC>(g_origGetDisplayModeList<Id>, self, format, flags, pNumModes, pDesc, "GetDisplayModeList", _ReturnAddress());
}

template <int Id>
HRESULT __stdcall hkFindClosest(IDXGIOutput* self, const DXGI_MODE_DESC* match, DXGI_MODE_DESC* closest, IUnknown* device) {
    return FindClosestInjected<DXGI_MODE_DESC>(g_origFindClosest<Id>, self, match, closest, device, "FindClosestMatchingMode", _ReturnAddress());
}

HRESULT __stdcall hkGetDisplayModeList1(IDXGIOutput1* self, DXGI_FORMAT format, UINT flags, UINT* pNumModes, DXGI_MODE_DESC1* pDesc) {
    return ExtendModeList<DXGI_MODE_DESC1>(g_origGetDisplayModeList1, self, format, flags, pNumModes, pDesc, "GetDisplayModeList1", _ReturnAddress());
}

HRESULT __stdcall hkFindClosest1(IDXGIOutput1* self, const DXGI_MODE_DESC1* match, DXGI_MODE_DESC1* closest, IUnknown* device) {
    return FindClosestInjected<DXGI_MODE_DESC1>(g_origFindClosest1, self, match, closest, device, "FindClosestMatchingMode1", _ReturnAddress());
}

// --- Swapchain / factory side ------------------------------------------------
// The game's own display-mode change (its resolution setting flows through
// ResizeTarget / CreateSwapChain) would hand the display a mode it can't do,
// so injected modes are swapped for the real current desktop mode on the way
// down; the backbuffer itself is then sized to the injected resolution via
// ResizeBuffers, which DXGI happily scales to the display.
constexpr size_t kSwapChainSetFullscreenStateIndex = 10;
constexpr size_t kSwapChainResizeBuffersIndex = 13;
constexpr size_t kSwapChainResizeTargetIndex = 14;
constexpr size_t kFactoryCreateSwapChainIndex = 10;
constexpr size_t kFactory2CreateSwapChainForHwndIndex = 15;

using ResizeTarget_t = HRESULT(__stdcall*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
using ResizeBuffers_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using SetFullscreenState_t = HRESULT(__stdcall*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using CreateSwapChain_t = HRESULT(__stdcall*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwnd_t = HRESULT(__stdcall*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

ResizeTarget_t g_origResizeTarget = nullptr;
ResizeBuffers_t g_origResizeBuffers = nullptr;
SetFullscreenState_t g_origSetFullscreenState = nullptr;
CreateSwapChain_t g_origCreateSwapChain = nullptr;
CreateSwapChainForHwnd_t g_origCreateSwapChainForHwnd = nullptr;

void CurrentDesktopMode(UINT* w, UINT* h) {
    RealDesktopMode(w, h);   // never the spoofed size: this feeds real display-mode changes
}

HRESULT __stdcall hkResizeTarget(IDXGISwapChain* self, const DXGI_MODE_DESC* p) {
    static std::atomic<int> n{0};
    if (p && n.fetch_add(1) < 12) {
        modlog::Line("SwapChainWatch: ResizeTarget " + std::to_string(p->Width) + "x" + std::to_string(p->Height)
            + " refresh " + std::to_string(p->RefreshRate.Numerator) + "/" + std::to_string(p->RefreshRate.Denominator)
            + " caller=" + CallerRva(_ReturnAddress()));
    }
    if (p && IsExtraMode(p->Width, p->Height)) {
        DXGI_MODE_DESC m = *p;
        CurrentDesktopMode(&m.Width, &m.Height);
        modlog::Line("SwapChainWatch: ResizeTarget remapped injected " + std::to_string(p->Width) + "x" + std::to_string(p->Height)
            + " -> real desktop mode " + std::to_string(m.Width) + "x" + std::to_string(m.Height));
        return g_origResizeTarget(self, &m);
    }
    return g_origResizeTarget(self, p);
}

HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    static std::atomic<int> n{0};
    LogVideoMemory(self, "before ResizeBuffers");
    HRESULT hr = g_origResizeBuffers(self, count, w, h, fmt, flags);
    LogVideoMemory(self, "after ResizeBuffers");
    if (n.fetch_add(1) < 12) {
        modlog::Line("SwapChainWatch: ResizeBuffers " + std::to_string(w) + "x" + std::to_string(h)
            + " count=" + std::to_string(count) + " -> hr=0x" + [&] { std::ostringstream o; o << std::hex << static_cast<unsigned>(hr); return o.str(); }()
            + " caller=" + CallerRva(_ReturnAddress()));
    }
    return hr;
}

HRESULT __stdcall hkSetFullscreenState(IDXGISwapChain* self, BOOL fullscreen, IDXGIOutput* target) {
    static std::atomic<int> n{0};
    HRESULT hr = g_origSetFullscreenState(self, fullscreen, target);
    if (n.fetch_add(1) < 12) {
        modlog::Line("SwapChainWatch: SetFullscreenState " + std::to_string(fullscreen) + " target=" + (target ? "set" : "null")
            + " -> hr=0x" + [&] { std::ostringstream o; o << std::hex << static_cast<unsigned>(hr); return o.str(); }()
            + " caller=" + CallerRva(_ReturnAddress()));
    }
    return hr;
}

HRESULT __stdcall hkCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppSwapChain) {
    OpenXRNoteSwapChainDevice(device);
    if (!desc) {
        return g_origCreateSwapChain(self, device, desc, ppSwapChain);
    }
    std::ostringstream oss;
    oss << "SwapChainWatch: CreateSwapChain " << desc->BufferDesc.Width << "x" << desc->BufferDesc.Height
        << " windowed=" << desc->Windowed << " swapEffect=" << desc->SwapEffect << " buffers=" << desc->BufferCount
        << " flags=0x" << std::hex << desc->Flags << " caller=" << CallerRva(_ReturnAddress());
    modlog::Line(oss.str());
    if (CalledFromGame(_ReturnAddress()) && desc->BufferDesc.Width >= 640) DesktopSpoofGameWindowCreated(desc->BufferDesc.Width, desc->BufferDesc.Height);

    // A *fullscreen* swapchain is created straight into a display mode, which
    // would fail for an injected size. Create it at the real desktop mode and
    // resize the buffers to the requested size afterwards. Windowed/borderless
    // swapchains take any size, so those are left alone.
    if (!desc->Windowed && IsExtraMode(desc->BufferDesc.Width, desc->BufferDesc.Height)) {
        UINT wantW = desc->BufferDesc.Width, wantH = desc->BufferDesc.Height;
        DXGI_SWAP_CHAIN_DESC d = *desc;
        CurrentDesktopMode(&d.BufferDesc.Width, &d.BufferDesc.Height);
        HRESULT hr = g_origCreateSwapChain(self, device, &d, ppSwapChain);
        if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
            HRESULT hr2 = (*ppSwapChain)->ResizeBuffers(0, wantW, wantH, DXGI_FORMAT_UNKNOWN, d.Flags);
            modlog::Line("SwapChainWatch: fullscreen create remapped to " + std::to_string(d.BufferDesc.Width) + "x"
                + std::to_string(d.BufferDesc.Height) + ", ResizeBuffers to " + std::to_string(wantW) + "x"
                + std::to_string(wantH) + " -> hr=0x" + [&] { std::ostringstream o; o << std::hex << static_cast<unsigned>(hr2); return o.str(); }());
        }
        return hr;
    }
    return g_origCreateSwapChain(self, device, desc, ppSwapChain);
}

HRESULT __stdcall hkCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrictTo, IDXGISwapChain1** pp) {
    OpenXRNoteSwapChainDevice(device);
    if (desc) {
        std::ostringstream oss;
        oss << "SwapChainWatch: CreateSwapChainForHwnd " << desc->Width << "x" << desc->Height
            << " fullscreenDesc=" << (fs ? "set" : "null") << " windowed=" << (fs ? fs->Windowed : 1)
            << " swapEffect=" << desc->SwapEffect << " scaling=" << desc->Scaling << " buffers=" << desc->BufferCount
            << " caller=" << CallerRva(_ReturnAddress());
        modlog::Line(oss.str());
        if (CalledFromGame(_ReturnAddress()) && desc->Width >= 640) DesktopSpoofGameWindowCreated(desc->Width, desc->Height);
    }
    return g_origCreateSwapChainForHwnd(self, device, hwnd, desc, fs, restrictTo, pp);
}

// The game's display-mode enumerator (FUN_140fc6020, the "Render: Enumerating
// Display Modes" pass) tags any mode larger than the current desktop with
// type 1 = "Surround", and the menu/launcher list builder (FUN_140fc7320)
// then drops type-1 modes unless NVIDIA Surround / AMD Eyefinity is present --
// which is why every injected mode above the desktop size was silently hidden
// even though it reached the game's internal list (confirmed via the game's
// own log). The tag is a single instruction:
//     CMP R8D,EAX ; JA tag ; MOV EAX,[RDX+54h] ; SUB EAX,[RDX+4Ch] ; CMP ECX,EAX
//     JBE skip ; tag: MOV dword ptr [RBP-1Dh], 1
// Turning that immediate 1 into 0 makes oversized modes ordinary modes.
bool PatchSurroundTag() {
    static const unsigned char kPattern[] = {
        0x44, 0x3B, 0xC0, 0x77, 0x0A, 0x8B, 0x42, 0x54, 0x2B, 0x42, 0x4C,
        0x3B, 0xC8, 0x76, 0x07, 0xC7, 0x45, 0xE3, 0x01, 0x00, 0x00, 0x00
    };
    constexpr size_t kImmediateOffset = 18;

    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        return false;
    }
    auto base = reinterpret_cast<uintptr_t>(exe);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage;

    auto* start = reinterpret_cast<unsigned char*>(base);
    unsigned char* found = nullptr;
    for (size_t i = 0; i + sizeof(kPattern) <= size; ++i) {
        if (start[i] == kPattern[0] && std::memcmp(start + i, kPattern, sizeof(kPattern)) == 0) {
            found = start + i;
            break;
        }
    }
    if (!found) {
        modlog::Line("ResolutionUnlock: Surround-tag pattern not found (different game build?) -- injected modes above the desktop size will stay hidden from the menu.");
        return false;
    }

    unsigned char* imm = found + kImmediateOffset;
    DWORD oldProtect = 0;
    if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        modlog::Line("ResolutionUnlock: VirtualProtect failed on the Surround-tag patch site.");
        return false;
    }
    *imm = 0x00;
    VirtualProtect(imm, 1, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), imm, 1);

    std::ostringstream oss;
    oss << "ResolutionUnlock: patched the Surround tag at SOTTR.exe+0x" << std::hex
        << (reinterpret_cast<uintptr_t>(found) - base) << " (oversized modes are no longer treated as Surround).";
    modlog::Line(oss.str());
    return true;
}

// --- Texture watch -----------------------------------------------------------
// Diagnostic: logs each distinct large 2D texture the game creates, to learn
// what it really allocates after a resolution change (the swapchain follows
// the monitor in fullscreen-window mode, so the internal render size is
// otherwise invisible to us). Resource creation is rare, so this is cheap.
constexpr size_t kDeviceCreateTexture2DIndex = 5;
using CreateTexture2D_t = HRESULT(__stdcall*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
CreateTexture2D_t g_origCreateTexture2D = nullptr;

HRESULT __stdcall hkCreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** pp) {
    if (desc && (desc->Width >= 2400 || desc->Height >= 2400)) {
        static std::mutex mtx;
        static std::vector<unsigned long long> seen;
        unsigned long long key = (static_cast<unsigned long long>(desc->Width) << 40) ^ (static_cast<unsigned long long>(desc->Height) << 20)
            ^ (static_cast<unsigned long long>(desc->Format) << 8) ^ desc->BindFlags ^ (static_cast<unsigned long long>(desc->ArraySize) << 60);
        std::lock_guard<std::mutex> lock(mtx);
        if (seen.size() < 90 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            std::ostringstream oss;
            oss << "TexWatch: " << desc->Width << "x" << desc->Height << " fmt=" << desc->Format
                << " bind=0x" << std::hex << desc->BindFlags << std::dec << " array=" << desc->ArraySize
                << " mips=" << desc->MipLevels << " msaa=" << desc->SampleDesc.Count
                << " caller=" << CallerRva(_ReturnAddress());
            modlog::Line(oss.str());
        }
    }
    return g_origCreateTexture2D(self, desc, init, pp);
}

bool InstallTextureWatch(ID3D11Device* device) {
    void** vt = *reinterpret_cast<void***>(device);
    if (MH_CreateHook(vt[kDeviceCreateTexture2DIndex], reinterpret_cast<void*>(&hkCreateTexture2D),
                      reinterpret_cast<void**>(&g_origCreateTexture2D)) != MH_OK
        || MH_EnableHook(vt[kDeviceCreateTexture2DIndex]) != MH_OK) {
        modlog::Line("TexWatch: CreateTexture2D hook failed.");
        return false;
    }
    return true;
}

bool HookOne(void* target, void* detour, void** original, const char* name) {
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        modlog::Line(std::string("ResolutionUnlock: hook setup failed for ") + name);
        return false;
    }
    return true;
}

// Finds any real IDXGIOutput on any adapter via a throwaway D3D11 device.
IDXGIOutput* FindAnyOutput() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(RealCreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        modlog::Line("ResolutionUnlock: could not create a DXGI factory to find an output.");
        return nullptr;
    }
    IDXGIOutput* found = nullptr;
    IDXGIAdapter* a = nullptr;
    for (UINT ai = 0; !found && factory->EnumAdapters(ai, &a) != DXGI_ERROR_NOT_FOUND; ++ai) {
        IDXGIOutput* o = nullptr;
        if (SUCCEEDED(a->EnumOutputs(0, &o)) && o) {
            found = o;
        }
        a->Release();
    }
    factory->Release();
    return found;
}

// Reads the swapchain and factory vtables from a throwaway swapchain, the same
// technique as the Present hook.
bool InstallSwapChainWatch() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VRModResUnlockDummyWindow";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        modlog::Line("SwapChainWatch: dummy window failed.");
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

    D3D_FEATURE_LEVEL fl;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &scd, &swapChain, &device, &fl, &context);
    if (FAILED(hr) || !swapChain) {
        modlog::Line("SwapChainWatch: throwaway swapchain failed.");
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    bool ok = true;
    ok &= InstallTextureWatch(device);
    void** sv = *reinterpret_cast<void***>(swapChain);
    ok &= HookOne(sv[kSwapChainResizeTargetIndex], reinterpret_cast<void*>(&hkResizeTarget),
                  reinterpret_cast<void**>(&g_origResizeTarget), "IDXGISwapChain::ResizeTarget");
    ok &= HookOne(sv[kSwapChainResizeBuffersIndex], reinterpret_cast<void*>(&hkResizeBuffers),
                  reinterpret_cast<void**>(&g_origResizeBuffers), "IDXGISwapChain::ResizeBuffers");
    ok &= HookOne(sv[kSwapChainSetFullscreenStateIndex], reinterpret_cast<void*>(&hkSetFullscreenState),
                  reinterpret_cast<void**>(&g_origSetFullscreenState), "IDXGISwapChain::SetFullscreenState");

    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(swapChain->GetParent(IID_PPV_ARGS(&factory))) && factory) {
        void** fv = *reinterpret_cast<void***>(factory);
        ok &= HookOne(fv[kFactoryCreateSwapChainIndex], reinterpret_cast<void*>(&hkCreateSwapChain),
                      reinterpret_cast<void**>(&g_origCreateSwapChain), "IDXGIFactory::CreateSwapChain");
        IDXGIFactory2* factory2 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) && factory2) {
            void** f2v = *reinterpret_cast<void***>(factory2);
            ok &= HookOne(f2v[kFactory2CreateSwapChainForHwndIndex], reinterpret_cast<void*>(&hkCreateSwapChainForHwnd),
                          reinterpret_cast<void**>(&g_origCreateSwapChainForHwnd), "IDXGIFactory2::CreateSwapChainForHwnd");
            factory2->Release();
        }
        factory->Release();
    }

    swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace

namespace {

bool InstallResolutionUnlockOnce();

} // namespace

// Idempotent, thread-safe and re-entrancy-safe. Called both from the init
// thread and -- the important one -- synchronously from inside the game's very
// first CreateDXGIFactory* call (see dxgi_exports.cpp), so the patch and hooks
// are guaranteed to be in place before the game's first display-mode
// enumeration. The game validates its saved resolution against that first
// list; losing that race made it reject the saved 5120x2880, fall back to
// 4096x2160 and turn stereo off. Creating the throwaway D3D11 device below
// re-enters CreateDXGIFactory* on this same thread, hence the guard.
bool InstallResolutionUnlock(bool blocking) {
    static std::mutex mtx;
    static bool done = false;
    static bool result = false;
    static thread_local bool installing = false;

    if (installing) {
        return true;
    }
    // The DXGI export wrappers can be entered from another injected DLL's
    // DllMain (OptiScaler calls CreateDXGIFactory there), i.e. with the loader
    // lock held. Blocking on our mutex then deadlocks against the init thread,
    // which needs the loader lock for its own work -- so exports never wait;
    // if the init thread is already installing, it will finish on its own.
    std::unique_lock<std::mutex> lock(mtx, std::defer_lock);
    if (blocking) {
        lock.lock();
    } else if (!lock.try_lock()) {
        return false;
    }
    if (done) {
        return result;
    }
    // 2026-09-25: the one attempt from the game's first CreateDXGIFactory found no output (the display wasn't reporting
    // for a moment -- a moment later it was), the unlock gave up for the session, and the game fell back to rendering
    // at 4096x2160 while the swapchain stayed 6368x4592. Now a failed attempt is retried: the export path leaves it
    // "not done" (the next call tries again, never waiting); the init thread retries every 100 ms for up to 3 s,
    // which is still before the game validates its resolution (~3 s after start).
    installing = true;
    bool r = InstallResolutionUnlockOnce();
    for (int i = 0; !r && blocking && i < 30; ++i) {
        Sleep(100);
        r = InstallResolutionUnlockOnce();
    }
    installing = false;
    if (r || blocking) {
        done = true;
        result = r;
    }
    if (r) modlog::Line("ResolutionUnlock: installed.");
    return r;
}

namespace {

bool InstallResolutionUnlockOnce() {
    if (!EnsureMinHookInitialized()) {
        modlog::Line("ResolutionUnlock: MinHook init failed.");
        return false;
    }
    static bool prepared = false;   // the patch and the desktop spoof run once; only finding an output is retried
    if (!prepared) {
        prepared = true;
        PatchSurroundTag();
        InstallDesktopSpoof();
    }

    // Device-free on purpose (this part can run from inside the game's or another
    // DLL's first CreateDXGIFactory call, possibly under the loader lock): the
    // output vtable comes from a plain factory, no D3D device or threads needed.
    IDXGIOutput* output = FindAnyOutput();
    if (!output) {
        static int misses = 0;
        if (++misses == 1 || misses % 10 == 0) modlog::Line("ResolutionUnlock: no IDXGIOutput found yet (attempt " + std::to_string(misses) + "); retrying.");
        return false;
    }
    DesktopSpoofHookOutput(output);

    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(IID_PPV_ARGS(&output1));

    void** vt = *reinterpret_cast<void***>(output);
    bool ok = true;
    ok &= HookOne(vt[kGetDisplayModeListIndex], reinterpret_cast<void*>(&hkGetDisplayModeList<0>),
                  reinterpret_cast<void**>(&g_origGetDisplayModeList<0>), "IDXGIOutput::GetDisplayModeList");
    ok &= HookOne(vt[kFindClosestMatchingModeIndex], reinterpret_cast<void*>(&hkFindClosest<0>),
                  reinterpret_cast<void**>(&g_origFindClosest<0>), "IDXGIOutput::FindClosestMatchingMode");

    if (output1) {
        void** vt1 = *reinterpret_cast<void***>(output1);
        if (vt1[kGetDisplayModeListIndex] != vt[kGetDisplayModeListIndex]) {
            ok &= HookOne(vt1[kGetDisplayModeListIndex], reinterpret_cast<void*>(&hkGetDisplayModeList<1>),
                          reinterpret_cast<void**>(&g_origGetDisplayModeList<1>), "IDXGIOutput1 slot 8");
        }
        if (vt1[kFindClosestMatchingModeIndex] != vt[kFindClosestMatchingModeIndex]) {
            ok &= HookOne(vt1[kFindClosestMatchingModeIndex], reinterpret_cast<void*>(&hkFindClosest<1>),
                          reinterpret_cast<void**>(&g_origFindClosest<1>), "IDXGIOutput1 slot 9");
        }
        ok &= HookOne(vt1[kGetDisplayModeList1Index], reinterpret_cast<void*>(&hkGetDisplayModeList1),
                      reinterpret_cast<void**>(&g_origGetDisplayModeList1), "IDXGIOutput1::GetDisplayModeList1");
        ok &= HookOne(vt1[kFindClosestMatchingMode1Index], reinterpret_cast<void*>(&hkFindClosest1),
                      reinterpret_cast<void**>(&g_origFindClosest1), "IDXGIOutput1::FindClosestMatchingMode1");
        output1->Release();
    }
    output->Release();

    modlog::Line(ok ? "ResolutionUnlock: display-mode hooks installed."
                    : "ResolutionUnlock: installed with some hooks failing (see above).");
    return ok;
}

} // namespace

// Swapchain/factory hooks need a throwaway swapchain (so a D3D11 device and a
// window); call from the init thread only, never from the DXGI export wrappers.
bool InstallSwapChainWatchLate() {
    modlog::Line("SwapChainWatch: waiting for the probe-device lock.");
    std::lock_guard<std::recursive_mutex> probeLock(ProbeDeviceMutex());
    modlog::Line("SwapChainWatch: creating throwaway swapchain.");
    bool ok = InstallSwapChainWatch();
    modlog::Line(ok ? "SwapChainWatch: hooks installed." : "SwapChainWatch: installation failed.");
    return ok;
}


namespace {
std::atomic<unsigned> g_gameModeW{ 0 }, g_gameModeH{ 0 };   // the game's own display mode (its log), 0 = not known yet
}

bool ResolutionGameMode(unsigned* w, unsigned* h) {
    *w = g_gameModeW.load();
    *h = g_gameModeH.load();
    return *w != 0;
}

void ResolutionCheckOnPresent() {
    static int presents = 0;
    static bool checked = false;
    if (checked || ++presents < 720) return;
    checked = true;
    unsigned fw = 0, fh = 0;
    if (!DesktopSpoofSize(&fw, &fh)) return;   // no fake desktop: nothing to compare
    const std::string settings = FrameLogReadGameSettings();
    const size_t at = settings.find("Display mode:");
    unsigned gw = 0, gh = 0;
    if (at == std::string::npos || sscanf_s(settings.c_str() + at, "Display mode: %ux%u", &gw, &gh) != 2) {
        modlog::Line("RESCHECK: the game's display mode wasn't found in its log; not checked.");
        return;
    }
    g_gameModeW.store(gw);
    g_gameModeH.store(gh);
    if (gw == fw && gh == fh) {
        modlog::Line("RESCHECK: the game renders at " + std::to_string(gw) + "x" + std::to_string(gh) + ", matching the fake desktop.");
        return;
    }
    modlog::Line("RESCHECK: WARNING -- the game settled on " + std::to_string(gw) + "x" + std::to_string(gh) + " but the fake desktop (and the "
        "swapchain) is " + std::to_string(fw) + "x" + std::to_string(fh) + ": the image is rendered small and stretched. Restart the game "
        "(or pick " + std::to_string(fw) + "x" + std::to_string(fh) + " in the display settings, or set Render resolution in the overlay).");
    // A first start (and a changed headset target) lands here as expected: the headset message asks for the restart.
    if (VRMOD_DIAG) PlayCue(Cue::Failure);
}

}
