#include "rt_logger.h"

#include <windows.h>
#include <d3d12.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>

#include "logger.h"
#include "present_hook.h"

namespace vrmod {

namespace {

// ID3D12Device vtable: 3 IUnknown + GetPrivateData(3) SetPrivateData(4) SetPrivateDataInterface(5) SetName(6)
// + GetNodeCount(7) CreateCommandQueue(8) ... CreateCommittedResource(27) CreateHeap(28) CreatePlacedResource(29)
// CreateReservedResource(30). Indices verified against d3d12.h in the SDK.
constexpr size_t kCreateCommittedResource = 27;
constexpr size_t kCreatePlacedResource = 29;
constexpr size_t kCreateReservedResource = 30;

using CreateCommitted_t = HRESULT(__stdcall*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreatePlaced_t = HRESULT(__stdcall*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateReserved_t = HRESULT(__stdcall*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);

CreateCommitted_t g_origCommitted = nullptr;
CreatePlaced_t g_origPlaced = nullptr;
CreateReserved_t g_origReserved = nullptr;

std::mutex g_mutex;
std::set<std::tuple<UINT64, UINT, int, int, UINT, UINT16, UINT16>> g_seen;
std::atomic<int> g_count{0};

void Note(const char* how, const D3D12_RESOURCE_DESC* d) {
    if (!d || d->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    const UINT wanted = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
        | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!(d->Flags & wanted) || d->Width < 256 || d->Height < 256) return;
    if (g_count.load() > 2500) return;
    const auto key = std::make_tuple(d->Width, d->Height, static_cast<int>(d->Format), static_cast<int>(d->Flags),
        static_cast<UINT>(d->DepthOrArraySize), d->MipLevels, static_cast<UINT16>(d->SampleDesc.Count));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_seen.insert(key).second) return;
    }
    ++g_count;
    std::ostringstream o;
    o << "RT " << how << " " << d->Width << "x" << d->Height << " fmt=" << d->Format
      << " arr=" << d->DepthOrArraySize << " mips=" << d->MipLevels << " msaa=" << d->SampleDesc.Count
      << " flags=" << ((d->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? "RT " : "")
      << ((d->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? "DS " : "")
      << ((d->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? "UAV" : "");
    modlog::Line(o.str());
}

HRESULT __stdcall hkCommitted(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID iid, void** pp) {
    Note("committed", d);
    return g_origCommitted(self, hp, hf, d, st, cv, iid, pp);
}

HRESULT __stdcall hkPlaced(ID3D12Device* self, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* d,
    D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID iid, void** pp) {
    Note("placed", d);
    return g_origPlaced(self, heap, off, d, st, cv, iid, pp);
}

HRESULT __stdcall hkReserved(ID3D12Device* self, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st,
    const D3D12_CLEAR_VALUE* cv, REFIID iid, void** pp) {
    Note("reserved", d);
    return g_origReserved(self, d, st, cv, iid, pp);
}

} // namespace

bool InstallRenderTargetLogger() {
    if (GetFileAttributesW(L"vrmod_rtlog.txt") == INVALID_FILE_ATTRIBUTES) return false;
    if (!EnsureMinHookInitialized()) return false;

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    ID3D12Device* device = nullptr;
    if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        modlog::Line("RT logger: couldn't create a temporary D3D12 device for the vtable.");
        return false;
    }
    void** vt = *reinterpret_cast<void***>(device);
    void* targets[3] = { vt[kCreateCommittedResource], vt[kCreatePlacedResource], vt[kCreateReservedResource] };
    device->Release();

    bool ok = MH_CreateHook(targets[0], &hkCommitted, reinterpret_cast<void**>(&g_origCommitted)) == MH_OK
        && MH_CreateHook(targets[1], &hkPlaced, reinterpret_cast<void**>(&g_origPlaced)) == MH_OK
        && MH_CreateHook(targets[2], &hkReserved, reinterpret_cast<void**>(&g_origReserved)) == MH_OK
        && MH_EnableHook(targets[0]) == MH_OK && MH_EnableHook(targets[1]) == MH_OK && MH_EnableHook(targets[2]) == MH_OK;
    modlog::Line(ok ? "RT logger: installed (vrmod_rtlog.txt present); logging large render targets as the game creates them."
                    : "RT logger: hook install failed.");
    return ok;
}

}
