#include "d3d12_cb_hook.h"

#include <windows.h>
#include <d3d12.h>
#include <MinHook.h>
#include <string>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <intrin.h>

#include "logger.h"

#pragma comment(lib, "d3d12.lib")

namespace vrmod {

namespace {

// All indices below verified directly from the Windows 10.0.26100.0 SDK's
// d3d12.h COBJMACROS sections (counted, not recalled from memory).
//
// ID3D12Resource: 0 QueryInterface, 1 AddRef, 2 Release, 3 GetPrivateData,
// 4 SetPrivateData, 5 SetPrivateDataInterface, 6 SetName, 7 GetDevice, 8 Map, 9 Unmap.
constexpr size_t kResourceMapIndex = 8;
constexpr size_t kResourceUnmapIndex = 9;

// ID3D12GraphicsCommandList: counted through GetType/Close/Reset/ClearState/
// Draw*/Dispatch/Copy*/ResolveSubresource/IASetPrimitiveTopology/RSSet*/
// OMSetBlendFactor/OMSetStencilRef/SetPipelineState/ResourceBarrier/
// ExecuteBundle/SetDescriptorHeaps/SetComputeRootSignature/SetGraphicsRootSignature
// -> SetComputeRootDescriptorTable=31, SetGraphicsRootDescriptorTable=32.
// Continuing: SetComputeRoot32BitConstant(33)/SetGraphicsRoot32BitConstant(34)/
// SetComputeRoot32BitConstants(35)/SetGraphicsRoot32BitConstants(36)/
// SetComputeRootConstantBufferView(37) -> SetGraphicsRootConstantBufferView=38.
constexpr size_t kSetGraphicsRootDescriptorTableIndex = 32;
constexpr size_t kSetGraphicsRoot32BitConstantsIndex = 36;
constexpr size_t kSetGraphicsRootCBVIndex = 38;

// ID3D12Device: counted through GetNodeCount/CreateCommandQueue/
// CreateCommandAllocator/CreateGraphicsPipelineState/CreateComputePipelineState/
// CreateCommandList/CheckFeatureSupport -> CreateDescriptorHeap=14,
// GetDescriptorHandleIncrementSize=15, CreateRootSignature=16,
// CreateConstantBufferView=17.
constexpr size_t kCreateDescriptorHeapIndex = 14;
constexpr size_t kCreateConstantBufferViewIndex = 17;
// ...CreateShaderResourceView(18)/CreateUnorderedAccessView(19)/CreateRenderTargetView(20)/
// CreateDepthStencilView(21)/CreateSampler(22) -> CopyDescriptors=23, CopyDescriptorsSimple=24.
constexpr size_t kCopyDescriptorsSimpleIndex = 24;

using Map12_t = HRESULT(__stdcall*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
using Unmap12_t = void(__stdcall*)(ID3D12Resource*, UINT, const D3D12_RANGE*);
using SetGraphicsRootCBV_t = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using SetGraphicsRootDescTable_t = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using SetGraphicsRoot32BitConstants_t = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using CreateDescriptorHeap_t = HRESULT(__stdcall*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using CreateConstantBufferView_t = void(__stdcall*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescriptorsSimple_t = void(__stdcall*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);

Map12_t oMap12 = nullptr;
Unmap12_t oUnmap12 = nullptr;
SetGraphicsRootCBV_t oSetGraphicsRootCBV = nullptr;
SetGraphicsRootDescTable_t oSetGraphicsRootDescTable = nullptr;
SetGraphicsRoot32BitConstants_t oSetGraphicsRoot32BitConstants = nullptr;
CreateDescriptorHeap_t oCreateDescriptorHeap = nullptr;
CreateConstantBufferView_t oCreateConstantBufferView = nullptr;
CopyDescriptorsSimple_t oCopyDescriptorsSimple = nullptr;

std::atomic<unsigned long long> g_mapCalls12{0};
std::atomic<unsigned long long> g_setCBVCalls{0};
std::atomic<unsigned long long> g_setDescTableCalls{0};
std::atomic<unsigned long long> g_registryLogCount{0};
std::atomic<unsigned long long> g_heapHits{0};
std::atomic<unsigned long long> g_cbvHits{0};
std::atomic<unsigned long long> g_resolveFails{0};

// How many bytes of a resolved buffer we'll ever actually print, independent
// of how large the source resource is.
constexpr UINT kMaxDumpBytes = 512;

// Call-site capture: logs the RVA (offset from the module base, so it's
// comparable to a Ghidra/IDA address across ASLR-randomized sessions) of the
// game code that called into a hooked function, the first time each distinct
// call site is seen. Millions of calls collapse down to a handful of unique
// call sites, which become direct targets for static decompilation instead
// of continuing to guess from runtime memory values.
std::mutex g_callSiteMutex;
std::unordered_set<uintptr_t> g_seenCallSites;

void LogCallSiteOnce(const char* hookName, void* returnAddress) {
    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t rva = reinterpret_cast<uintptr_t>(returnAddress) - moduleBase;

    bool isNew = false;
    {
        std::lock_guard<std::mutex> lock(g_callSiteMutex);
        isNew = g_seenCallSites.insert(rva).second;
    }

    if (isNew) {
        std::ostringstream oss;
        oss << "CALLSITE " << hookName << " rva=0x" << std::hex << rva << std::dec;
        modlog::Line(oss.str());
    }
}

// Resource registry: GPU virtual address -> CPU-readable pointer, built from
// every ID3D12Resource::Map call regardless of size (a size filter here is
// exactly what caused us to miss the real per-frame constant "arena" buffer
// the first time around -- it's much larger than a handful of matrices).
struct TrackedResource {
    D3D12_GPU_VIRTUAL_ADDRESS gpuBase;
    void* cpuBase;
    UINT64 size;
};
std::mutex g_resourceMutex;
std::unordered_map<ID3D12Resource*, TrackedResource> g_resourceRegistry;

// Descriptor heap registry: lets us translate a GPU-visible descriptor handle
// (as seen in SetGraphicsRootDescriptorTable) back to the matching CPU-visible
// handle in the same heap, since CBV/SRV/UAV descriptor heaps mirror the same
// slot layout in both address spaces, just with different base addresses.
struct TrackedHeap {
    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase;
    UINT incrementSize;
    UINT numDescriptors;
};
std::mutex g_heapMutex;
std::unordered_map<ID3D12DescriptorHeap*, TrackedHeap> g_heapRegistry;

// CBV registry: CPU descriptor handle -> the GPU virtual address and size that
// were written into that descriptor slot by CreateConstantBufferView. This is
// the actual "what buffer does this descriptor point at" lookup.
struct TrackedCBV {
    D3D12_GPU_VIRTUAL_ADDRESS bufferLocation;
    UINT sizeInBytes;
};
std::mutex g_cbvMutex;
std::unordered_map<SIZE_T, TrackedCBV> g_cbvRegistry;

void DumpBuffer(const char* tag, ID3D12Resource* res, void* data, UINT64 byteWidth, UINT rootParamIndex) {
    UINT bytesToPrint = static_cast<UINT>((byteWidth < kMaxDumpBytes) ? byteWidth : kMaxDumpBytes);
    size_t floatCount = bytesToPrint / sizeof(float);
    const float* floats = reinterpret_cast<const float*>(data);

    std::ostringstream oss;
    oss << tag << " rootParam=" << rootParamIndex << " res=" << res
        << " bytes=" << byteWidth << " floats=[";
    oss << std::fixed << std::setprecision(4);
    for (size_t i = 0; i < floatCount; ++i) {
        if (i > 0) {
            oss << ((i % 4 == 0) ? " | " : " ");
        }
        oss << floats[i];
    }
    oss << "]";

    modlog::Line(oss.str());
}

// Given a GPU virtual address, finds the tracked resource whose mapped range
// contains it and returns a CPU-readable pointer + remaining byte count.
bool ResolveGpuVA(D3D12_GPU_VIRTUAL_ADDRESS bufferLocation, ID3D12Resource** outRes, void** outPtr, UINT64* outRemaining) {
    std::lock_guard<std::mutex> lock(g_resourceMutex);
    for (auto& kv : g_resourceRegistry) {
        const TrackedResource& info = kv.second;
        if (bufferLocation >= info.gpuBase && bufferLocation < info.gpuBase + info.size) {
            *outRes = kv.first;
            *outPtr = reinterpret_cast<uint8_t*>(info.cpuBase) + (bufferLocation - info.gpuBase);
            *outRemaining = info.size - (bufferLocation - info.gpuBase);
            return true;
        }
    }
    return false;
}

HRESULT __stdcall hkMap12(ID3D12Resource* self, UINT subresource, const D3D12_RANGE* pReadRange, void** ppData) {
    LogCallSiteOnce("Map", _ReturnAddress());

    HRESULT hr = oMap12(self, subresource, pReadRange, ppData);

    auto count = g_mapCalls12.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 5000 == 0) {
        modlog::Line("D3D12 hook: ID3D12Resource::Map called, count=" + std::to_string(count));
    }

    if (SUCCEEDED(hr) && self && ppData && *ppData) {
        D3D12_RESOURCE_DESC desc = self->GetDesc();
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            std::lock_guard<std::mutex> lock(g_resourceMutex);
            g_resourceRegistry[self] = { self->GetGPUVirtualAddress(), *ppData, desc.Width };
        }
    }

    return hr;
}

void __stdcall hkUnmap12(ID3D12Resource* self, UINT subresource, const D3D12_RANGE* pWrittenRange) {
    // Deliberately not erasing the resource registry entry -- see earlier
    // comment history in this project; upload-heap CPU pointers stay valid
    // and readable well past Unmap in practice, and bindings often reference
    // the buffer later in the same frame's command list.
    oUnmap12(self, subresource, pWrittenRange);
}

void __stdcall hkSetGraphicsRootCBV(ID3D12GraphicsCommandList* self, UINT rootParamIndex,
                                     D3D12_GPU_VIRTUAL_ADDRESS bufferLocation) {
    LogCallSiteOnce("SetGraphicsRootConstantBufferView", _ReturnAddress());

    auto count = g_setCBVCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 2000 == 0) {
        modlog::Line("D3D12 hook: SetGraphicsRootConstantBufferView called, count=" + std::to_string(count));
    }

    if (bufferLocation != 0 && (GetAsyncKeyState(VK_F9) & 0x8000)) {
        ID3D12Resource* res = nullptr;
        void* dataPtr = nullptr;
        UINT64 remaining = 0;
        if (ResolveGpuVA(bufferLocation, &res, &dataPtr, &remaining)) {
            DumpBuffer("CBV", res, dataPtr, remaining, rootParamIndex);
        }
    }

    oSetGraphicsRootCBV(self, rootParamIndex, bufferLocation);
}

void __stdcall hkSetGraphicsRootDescTable(ID3D12GraphicsCommandList* self, UINT rootParamIndex,
                                           D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor) {
    LogCallSiteOnce("SetGraphicsRootDescriptorTable", _ReturnAddress());

    auto count = g_setDescTableCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 2000 == 0) {
        modlog::Line("D3D12 hook: SetGraphicsRootDescriptorTable called, count=" + std::to_string(count)
            + " heapHits=" + std::to_string(g_heapHits.load()) + " cbvHits=" + std::to_string(g_cbvHits.load())
            + " resolveFails=" + std::to_string(g_resolveFails.load()));
    }

    // Run the resolution attempt unconditionally (not just while F9 is down) so
    // we get real hit-rate diagnostics regardless of when the hotkey is pressed.
    if (baseDescriptor.ptr != 0) {
        SIZE_T resolvedCpuHandle = 0;
        bool foundHeap = false;
        {
            std::lock_guard<std::mutex> lock(g_heapMutex);
            for (auto& kv : g_heapRegistry) {
                const TrackedHeap& heap = kv.second;
                UINT64 heapSpan = static_cast<UINT64>(heap.incrementSize) * heap.numDescriptors;
                if (baseDescriptor.ptr >= heap.gpuBase.ptr && baseDescriptor.ptr < heap.gpuBase.ptr + heapSpan) {
                    UINT64 slot = (baseDescriptor.ptr - heap.gpuBase.ptr) / heap.incrementSize;
                    resolvedCpuHandle = heap.cpuBase.ptr + static_cast<SIZE_T>(slot) * heap.incrementSize;
                    foundHeap = true;
                    break;
                }
            }
        }
        if (foundHeap) {
            g_heapHits.fetch_add(1, std::memory_order_relaxed);
        }

        if (foundHeap) {
            TrackedCBV cbv{};
            bool foundCBV = false;
            {
                std::lock_guard<std::mutex> lock(g_cbvMutex);
                auto it = g_cbvRegistry.find(resolvedCpuHandle);
                if (it != g_cbvRegistry.end()) {
                    cbv = it->second;
                    foundCBV = true;
                }
            }
            if (foundCBV) {
                g_cbvHits.fetch_add(1, std::memory_order_relaxed);
            }

            if (foundCBV) {
                ID3D12Resource* res = nullptr;
                void* dataPtr = nullptr;
                UINT64 remaining = 0;
                bool resolved = ResolveGpuVA(cbv.bufferLocation, &res, &dataPtr, &remaining);
                if (!resolved) {
                    g_resolveFails.fetch_add(1, std::memory_order_relaxed);
                } else if (GetAsyncKeyState(VK_F9) & 0x8000) {
                    UINT64 size = (cbv.sizeInBytes < remaining) ? cbv.sizeInBytes : remaining;
                    DumpBuffer("CBVTABLE", res, dataPtr, size, rootParamIndex);
                }
            }
        }
    }

    oSetGraphicsRootDescTable(self, rootParamIndex, baseDescriptor);
}

std::atomic<unsigned long long> g_set32BitCalls{0};

// Root 32-bit constants are passed directly by value (the game just wrote
// them into a local buffer and gives us a pointer to it for the API to copy
// from) -- unlike Map/CBV data, there's no GPU-memory resolution needed at
// all, we can read pSrcData immediately.
void __stdcall hkSetGraphicsRoot32BitConstants(ID3D12GraphicsCommandList* self, UINT rootParamIndex,
                                                UINT num32BitValues, const void* pSrcData,
                                                UINT destOffsetIn32BitValues) {
    LogCallSiteOnce("SetGraphicsRoot32BitConstants", _ReturnAddress());

    auto count = g_set32BitCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 2000 == 0) {
        modlog::Line("D3D12 hook: SetGraphicsRoot32BitConstants called, count=" + std::to_string(count));
    }

    if (pSrcData && (GetAsyncKeyState(VK_F9) & 0x8000)) {
        const float* floats = reinterpret_cast<const float*>(pSrcData);
        std::ostringstream oss;
        oss << "ROOTCONST rootParam=" << rootParamIndex << " num=" << num32BitValues
            << " destOffset=" << destOffsetIn32BitValues << " floats=[";
        oss << std::fixed << std::setprecision(4);
        for (UINT i = 0; i < num32BitValues; ++i) {
            if (i > 0) {
                oss << " ";
            }
            oss << floats[i];
        }
        oss << "]";
        modlog::Line(oss.str());
    }

    oSetGraphicsRoot32BitConstants(self, rootParamIndex, num32BitValues, pSrcData, destOffsetIn32BitValues);
}

HRESULT __stdcall hkCreateDescriptorHeap(ID3D12Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc,
                                          REFIID riid, void** ppvHeap) {
    HRESULT hr = oCreateDescriptorHeap(self, pDesc, riid, ppvHeap);

    if (SUCCEEDED(hr) && pDesc && ppvHeap && *ppvHeap
        && pDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
        && (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) {
        auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(*ppvHeap);
        TrackedHeap info{};
        info.cpuBase = heap->GetCPUDescriptorHandleForHeapStart();
        info.gpuBase = heap->GetGPUDescriptorHandleForHeapStart();
        info.incrementSize = self->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        info.numDescriptors = pDesc->NumDescriptors;

        std::lock_guard<std::mutex> lock(g_heapMutex);
        g_heapRegistry[heap] = info;

        modlog::Line("D3D12 hook: tracked shader-visible CBV/SRV/UAV heap, numDescriptors=" + std::to_string(info.numDescriptors));
    }

    return hr;
}

std::atomic<unsigned long long> g_createCBVCalls{0};

void __stdcall hkCreateConstantBufferView(ID3D12Device* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
    auto count = g_createCBVCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 200 == 0) {
        modlog::Line("D3D12 hook: CreateConstantBufferView called, count=" + std::to_string(count));
    }

    if (pDesc) {
        std::lock_guard<std::mutex> lock(g_cbvMutex);
        g_cbvRegistry[destDescriptor.ptr] = { pDesc->BufferLocation, pDesc->SizeInBytes };
    }

    oCreateConstantBufferView(self, pDesc, destDescriptor);
}

std::atomic<unsigned long long> g_copyDescriptorsCalls{0};
std::atomic<unsigned long long> g_copyPropagations{0};

void __stdcall hkCopyDescriptorsSimple(ID3D12Device* self, UINT numDescriptors,
                                        D3D12_CPU_DESCRIPTOR_HANDLE destStart,
                                        D3D12_CPU_DESCRIPTOR_HANDLE srcStart,
                                        D3D12_DESCRIPTOR_HEAP_TYPE type) {
    auto count = g_copyDescriptorsCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 2000 == 0) {
        modlog::Line("D3D12 hook: CopyDescriptorsSimple called, count=" + std::to_string(count)
            + " propagations=" + std::to_string(g_copyPropagations.load()));
    }

    // If the source descriptor(s) are CBVs we already know about, propagate
    // that knowledge to the destination handle(s) -- this is what lets us
    // resolve a bindless-heap slot that was filled via a copy from a staging
    // heap rather than a direct CreateConstantBufferView onto that slot.
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        UINT increment = self->GetDescriptorHandleIncrementSize(type);
        std::lock_guard<std::mutex> lock(g_cbvMutex);
        for (UINT i = 0; i < numDescriptors; ++i) {
            SIZE_T srcHandle = srcStart.ptr + static_cast<SIZE_T>(i) * increment;
            SIZE_T destHandle = destStart.ptr + static_cast<SIZE_T>(i) * increment;
            auto it = g_cbvRegistry.find(srcHandle);
            if (it != g_cbvRegistry.end()) {
                TrackedCBV value = it->second;
                g_cbvRegistry[destHandle] = value;
                g_copyPropagations.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    oCopyDescriptorsSimple(self, numDescriptors, destStart, srcStart, type);
}

// Creates a throwaway D3D12 device + upload-heap buffer resource + command
// allocator/list solely to read real function addresses out of the relevant
// vtables. Same technique as GetRealAddresses() in present_hook.cpp.
bool GetRealAddresses(void** outMapAddr, void** outUnmapAddr, void** outSetCBVAddr,
                       void** outSetDescTableAddr, void** outCreateHeapAddr, void** outCreateCBVAddr,
                       void** outCopyDescriptorsSimpleAddr, void** outSet32BitConstantsAddr) {
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        modlog::Line("D3D12 hook: D3D12CreateDevice failed, hr=" + std::to_string(hr));
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width = 256;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* resource = nullptr;
    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr) || !resource) {
        modlog::Line("D3D12 hook: CreateCommittedResource failed, hr=" + std::to_string(hr));
        device->Release();
        return false;
    }

    ID3D12CommandAllocator* allocator = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr) || !allocator) {
        modlog::Line("D3D12 hook: CreateCommandAllocator failed, hr=" + std::to_string(hr));
        resource->Release();
        device->Release();
        return false;
    }

    ID3D12GraphicsCommandList* cmdList = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmdList));
    if (FAILED(hr) || !cmdList) {
        modlog::Line("D3D12 hook: CreateCommandList failed, hr=" + std::to_string(hr));
        allocator->Release();
        resource->Release();
        device->Release();
        return false;
    }

    void** resourceVTable = *reinterpret_cast<void***>(resource);
    *outMapAddr = resourceVTable[kResourceMapIndex];
    *outUnmapAddr = resourceVTable[kResourceUnmapIndex];

    void** cmdListVTable = *reinterpret_cast<void***>(cmdList);
    *outSetCBVAddr = cmdListVTable[kSetGraphicsRootCBVIndex];
    *outSetDescTableAddr = cmdListVTable[kSetGraphicsRootDescriptorTableIndex];
    *outSet32BitConstantsAddr = cmdListVTable[kSetGraphicsRoot32BitConstantsIndex];

    void** deviceVTable = *reinterpret_cast<void***>(device);
    *outCreateHeapAddr = deviceVTable[kCreateDescriptorHeapIndex];
    *outCreateCBVAddr = deviceVTable[kCreateConstantBufferViewIndex];
    *outCopyDescriptorsSimpleAddr = deviceVTable[kCopyDescriptorsSimpleIndex];

    cmdList->Release();
    allocator->Release();
    resource->Release();
    device->Release();
    return true;
}

} // namespace

bool InstallD3D12ConstantBufferHook() {
    void* mapAddr = nullptr;
    void* unmapAddr = nullptr;
    void* setCBVAddr = nullptr;
    void* setDescTableAddr = nullptr;
    void* createHeapAddr = nullptr;
    void* createCBVAddr = nullptr;
    void* copyDescriptorsSimpleAddr = nullptr;
    void* set32BitConstantsAddr = nullptr;
    if (!GetRealAddresses(&mapAddr, &unmapAddr, &setCBVAddr, &setDescTableAddr, &createHeapAddr, &createCBVAddr,
                           &copyDescriptorsSimpleAddr, &set32BitConstantsAddr)) {
        return false;
    }

    // MH_Initialize() is already called by InstallPresentHook() before this runs;
    // MinHook supports multiple CreateHook calls against one initialized instance.
    struct HookSpec {
        void* target;
        void* detour;
        void** original;
        const char* name;
    };
    HookSpec hooks[] = {
        { mapAddr, &hkMap12, reinterpret_cast<void**>(&oMap12), "Map" },
        { unmapAddr, &hkUnmap12, reinterpret_cast<void**>(&oUnmap12), "Unmap" },
        { setCBVAddr, &hkSetGraphicsRootCBV, reinterpret_cast<void**>(&oSetGraphicsRootCBV), "SetGraphicsRootConstantBufferView" },
        { setDescTableAddr, &hkSetGraphicsRootDescTable, reinterpret_cast<void**>(&oSetGraphicsRootDescTable), "SetGraphicsRootDescriptorTable" },
        { createHeapAddr, &hkCreateDescriptorHeap, reinterpret_cast<void**>(&oCreateDescriptorHeap), "CreateDescriptorHeap" },
        { createCBVAddr, &hkCreateConstantBufferView, reinterpret_cast<void**>(&oCreateConstantBufferView), "CreateConstantBufferView" },
        { copyDescriptorsSimpleAddr, &hkCopyDescriptorsSimple, reinterpret_cast<void**>(&oCopyDescriptorsSimple), "CopyDescriptorsSimple" },
        { set32BitConstantsAddr, &hkSetGraphicsRoot32BitConstants, reinterpret_cast<void**>(&oSetGraphicsRoot32BitConstants), "SetGraphicsRoot32BitConstants" },
    };

    for (const auto& h : hooks) {
        if (MH_CreateHook(h.target, h.detour, h.original) != MH_OK || MH_EnableHook(h.target) != MH_OK) {
            modlog::Line(std::string("D3D12 hook: ") + h.name + " hook setup failed.");
            return false;
        }
    }

    modlog::Line("D3D12 hook: all hooks installed (Map/Unmap, root CBV, root descriptor table, "
                  "CreateDescriptorHeap, CreateConstantBufferView). Press F9 in-game to dump resolved buffers.");
    return true;
}

} // namespace vrmod
