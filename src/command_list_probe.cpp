#include "command_list_probe.h"

#include <windows.h>
#include <d3d12.h>
#include <MinHook.h>
#include <string>
#include <sstream>
#include <mutex>
#include <atomic>
#include <vector>
#include <unordered_map>

#include "logger.h"

#pragma comment(lib, "d3d12.lib")

namespace vrmod {

namespace {

// All indices verified directly from the Windows 10.0.26100.0 SDK's d3d12.h
// COBJMACROS sections (counted, not recalled from memory) -- same discipline
// as d3d12_cb_hook.cpp.
//
// ID3D12Device: ...CreateCommandAllocator(9)/CreateGraphicsPipelineState(10)/
// CreateComputePipelineState(11)/CreateCommandList(12)/CheckFeatureSupport(13)/
// CreateDescriptorHeap(14)/GetDescriptorHandleIncrementSize(15)/
// CreateRootSignature(16)/CreateConstantBufferView(17)/CreateShaderResourceView(18)/
// CreateUnorderedAccessView(19) -> CreateRenderTargetView=20.
// (GetNodeCount=7, CreateCommandQueue=8.)
constexpr size_t kCreateCommandQueueIndex = 8;
constexpr size_t kCreateRenderTargetViewIndex = 20;

// ID3D12CommandQueue: QueryInterface..GetDevice(0-7), UpdateTileMappings(8),
// CopyTileMappings(9) -> ExecuteCommandLists=10.
constexpr size_t kExecuteCommandListsIndex = 10;

// ID3D12GraphicsCommandList: QueryInterface..GetDevice(0-7), GetType(8),
// Close=9, Reset=10, ClearState(11), DrawInstanced=12, DrawIndexedInstanced=13,
// ... OMSetRenderTargets=46 (counted the same way as the SetGraphicsRoot*
// indices already verified in d3d12_cb_hook.cpp, continuing the same list).
constexpr size_t kCloseIndex = 9;
constexpr size_t kResetIndex = 10;
constexpr size_t kDrawInstancedIndex = 12;
constexpr size_t kDrawIndexedInstancedIndex = 13;
constexpr size_t kOMSetRenderTargetsIndex = 46;

using CreateRenderTargetView_t = void(__stdcall*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using ExecuteCommandLists_t = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using Reset_t = HRESULT(__stdcall*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using Close_t = HRESULT(__stdcall*)(ID3D12GraphicsCommandList*);
using OMSetRenderTargets_t = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using DrawInstanced_t = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexedInstanced_t = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT, INT);

CreateRenderTargetView_t oCreateRenderTargetView = nullptr;
ExecuteCommandLists_t oExecuteCommandLists = nullptr;
Reset_t oReset = nullptr;
Close_t oClose = nullptr;
OMSetRenderTargets_t oOMSetRenderTargets = nullptr;
DrawInstanced_t oDrawInstanced = nullptr;
DrawIndexedInstanced_t oDrawIndexedInstanced = nullptr;

// One-shot, single-frame capture. Armed by F10; captures everything recorded
// between the Present call that arms it and the next Present call, then
// flushes the whole thing to vrmod.log at once. Kept off (atomic checks only)
// the rest of the time so these very hot hooks stay cheap.
std::atomic<bool> g_captureActive{false};
std::mutex g_captureLogMutex;
std::vector<std::string> g_captureLog;

void CaptureLine(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_captureLogMutex);
    g_captureLog.push_back(s);
}

// Resource behind each RTV descriptor handle, so pass summaries can show
// resolution/format instead of just an opaque handle value -- e.g. a
// 1920x1080 R16G16B16A16_FLOAT target is almost certainly HDR scene color,
// while something matching the backbuffer's exact format/size feeding only a
// handful of draws is more likely a UI composite pass.
struct RTVInfo {
    ID3D12Resource* resource = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
std::mutex g_rtvMutex;
std::unordered_map<SIZE_T, RTVInfo> g_rtvRegistry;

struct CmdListPassInfo {
    SIZE_T firstRtvHandle = 0;
    UINT rtvCount = 0;
    bool hasDsv = false;
    UINT64 drawCount = 0;
};

struct CmdListState {
    std::vector<std::string> passSummaries;
    CmdListPassInfo current;
    bool hasCurrent = false;
};
std::mutex g_cmdListStateMutex;
std::unordered_map<ID3D12GraphicsCommandList*, CmdListState> g_cmdListStates;

// Unconditional heartbeat counters (logged regardless of capture state), same
// convention as d3d12_cb_hook.cpp -- these are the only way to confirm these
// specific hooks are actually intercepting real game calls at all, since the
// capture-window logging above only fires while g_captureActive is true and
// would otherwise stay silent even if the hooked addresses were wrong.
std::atomic<unsigned long long> g_executeCalls{0};
std::atomic<unsigned long long> g_resetCalls{0};
std::atomic<unsigned long long> g_closeCalls{0};
std::atomic<unsigned long long> g_omSetRTCalls{0};
std::atomic<unsigned long long> g_drawCalls{0};

// Closes out the pass that was open (if any), appending a one-line summary to
// the command list's pass list. Called when OMSetRenderTargets switches to a
// new target set, and again on Close() for whatever pass was open last.
void FlushCurrentPass(CmdListState& state) {
    if (!state.hasCurrent) {
        return;
    }

    std::ostringstream oss;
    oss << "rt=0x" << std::hex << state.current.firstRtvHandle << std::dec
        << " count=" << state.current.rtvCount
        << " dsv=" << (state.current.hasDsv ? "y" : "n")
        << " draws=" << state.current.drawCount;

    RTVInfo info{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_rtvMutex);
        auto it = g_rtvRegistry.find(state.current.firstRtvHandle);
        if (it != g_rtvRegistry.end()) {
            info = it->second;
            found = true;
        }
    }
    if (found) {
        oss << " res=" << info.resource << " " << info.width << "x" << info.height
            << " fmt=" << static_cast<int>(info.format);
    }

    state.passSummaries.push_back(oss.str());
    state.hasCurrent = false;
}

void __stdcall hkCreateRenderTargetView(ID3D12Device* self, ID3D12Resource* pResource,
                                         const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                         D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
    if (pResource) {
        D3D12_RESOURCE_DESC desc = pResource->GetDesc();
        std::lock_guard<std::mutex> lock(g_rtvMutex);
        g_rtvRegistry[destDescriptor.ptr] = { pResource, static_cast<UINT>(desc.Width), desc.Height, desc.Format };
    }
    oCreateRenderTargetView(self, pResource, pDesc, destDescriptor);
}

void __stdcall hkExecuteCommandLists(ID3D12CommandQueue* self, UINT numLists, ID3D12CommandList* const* ppLists) {
    auto count = g_executeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 500 == 0) {
        modlog::Line("CommandListProbe: ExecuteCommandLists called, count=" + std::to_string(count));
    }
    if (g_captureActive.load(std::memory_order_relaxed)) {
        std::ostringstream oss;
        oss << "EXECUTE queue=" << self << " count=" << numLists << " lists=[";
        for (UINT i = 0; i < numLists; ++i) {
            if (i) oss << ",";
            oss << ppLists[i];
        }
        oss << "]";
        CaptureLine(oss.str());
    }
    oExecuteCommandLists(self, numLists, ppLists);
}

HRESULT __stdcall hkReset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    HRESULT hr = oReset(self, allocator, pso);
    auto count = g_resetCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 5000 == 0) {
        modlog::Line("CommandListProbe: Reset called, count=" + std::to_string(count));
    }
    if (g_captureActive.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_cmdListStateMutex);
        g_cmdListStates[self] = CmdListState{};
    }
    return hr;
}

HRESULT __stdcall hkClose(ID3D12GraphicsCommandList* self) {
    auto count = g_closeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 5000 == 0) {
        modlog::Line("CommandListProbe: Close called, count=" + std::to_string(count));
    }
    if (g_captureActive.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_cmdListStateMutex);
        auto it = g_cmdListStates.find(self);
        if (it != g_cmdListStates.end()) {
            FlushCurrentPass(it->second);
            std::ostringstream oss;
            oss << "CMDLIST " << self << " closed, " << it->second.passSummaries.size() << " passes:";
            for (auto& p : it->second.passSummaries) {
                oss << " |" << p;
            }
            CaptureLine(oss.str());
        }
    }
    return oClose(self);
}

void __stdcall hkOMSetRenderTargets(ID3D12GraphicsCommandList* self, UINT numRT,
                                     const D3D12_CPU_DESCRIPTOR_HANDLE* pRTs, BOOL singleHandle,
                                     const D3D12_CPU_DESCRIPTOR_HANDLE* pDSV) {
    auto rtCount = g_omSetRTCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rtCount == 1 || rtCount % 5000 == 0) {
        modlog::Line("CommandListProbe: OMSetRenderTargets called, count=" + std::to_string(rtCount));
    }
    if (g_captureActive.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_cmdListStateMutex);
        auto& state = g_cmdListStates[self];
        FlushCurrentPass(state);
        state.current = CmdListPassInfo{};
        state.hasCurrent = true;
        if (pRTs && numRT > 0) {
            state.current.firstRtvHandle = pRTs[0].ptr;
            state.current.rtvCount = numRT;
        }
        state.current.hasDsv = (pDSV != nullptr);
    }
    oOMSetRenderTargets(self, numRT, pRTs, singleHandle, pDSV);
}

void __stdcall hkDrawInstanced(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, UINT d) {
    auto count = g_drawCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 20000 == 0) {
        modlog::Line("CommandListProbe: Draw(Instanced+Indexed) called, count=" + std::to_string(count));
    }
    if (g_captureActive.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_cmdListStateMutex);
        auto it = g_cmdListStates.find(self);
        if (it != g_cmdListStates.end() && it->second.hasCurrent) {
            ++it->second.current.drawCount;
        }
    }
    oDrawInstanced(self, a, b, c, d);
}

void __stdcall hkDrawIndexedInstanced(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, INT d, UINT e) {
    auto count = g_drawCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 20000 == 0) {
        modlog::Line("CommandListProbe: Draw(Instanced+Indexed) called, count=" + std::to_string(count));
    }
    if (g_captureActive.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_cmdListStateMutex);
        auto it = g_cmdListStates.find(self);
        if (it != g_cmdListStates.end() && it->second.hasCurrent) {
            ++it->second.current.drawCount;
        }
    }
    oDrawIndexedInstanced(self, a, b, c, d, e);
}

// Creates throwaway D3D12 objects solely to read real function addresses out
// of their vtables -- same technique as GetRealAddresses() in
// d3d12_cb_hook.cpp and present_hook.cpp.
bool GetRealAddresses(void** outCreateRTVAddr, void** outExecuteAddr, void** outResetAddr,
                       void** outCloseAddr, void** outOMSetRTAddr, void** outDrawAddr,
                       void** outDrawIndexedAddr) {
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        modlog::Line("CommandListProbe: D3D12CreateDevice failed, hr=" + std::to_string(hr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
    if (FAILED(hr) || !rtvHeap) {
        modlog::Line("CommandListProbe: CreateDescriptorHeap(RTV) failed, hr=" + std::to_string(hr));
        device->Release();
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 4;
    texDesc.Height = 4;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* rtResource = nullptr;
    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rtResource));
    if (FAILED(hr) || !rtResource) {
        modlog::Line("CommandListProbe: CreateCommittedResource(RT) failed, hr=" + std::to_string(hr));
        rtvHeap->Release();
        device->Release();
        return false;
    }

    device->CreateRenderTargetView(rtResource, nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr) || !queue) {
        modlog::Line("CommandListProbe: CreateCommandQueue failed, hr=" + std::to_string(hr));
        rtResource->Release();
        rtvHeap->Release();
        device->Release();
        return false;
    }

    ID3D12CommandAllocator* allocator = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr) || !allocator) {
        modlog::Line("CommandListProbe: CreateCommandAllocator failed, hr=" + std::to_string(hr));
        queue->Release();
        rtResource->Release();
        rtvHeap->Release();
        device->Release();
        return false;
    }

    ID3D12GraphicsCommandList* cmdList = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmdList));
    if (FAILED(hr) || !cmdList) {
        modlog::Line("CommandListProbe: CreateCommandList failed, hr=" + std::to_string(hr));
        allocator->Release();
        queue->Release();
        rtResource->Release();
        rtvHeap->Release();
        device->Release();
        return false;
    }

    void** deviceVTable = *reinterpret_cast<void***>(device);
    *outCreateRTVAddr = deviceVTable[kCreateRenderTargetViewIndex];

    void** queueVTable = *reinterpret_cast<void***>(queue);
    *outExecuteAddr = queueVTable[kExecuteCommandListsIndex];

    void** cmdListVTable = *reinterpret_cast<void***>(cmdList);
    *outResetAddr = cmdListVTable[kResetIndex];
    *outCloseAddr = cmdListVTable[kCloseIndex];
    *outOMSetRTAddr = cmdListVTable[kOMSetRenderTargetsIndex];
    *outDrawAddr = cmdListVTable[kDrawInstancedIndex];
    *outDrawIndexedAddr = cmdListVTable[kDrawIndexedInstancedIndex];

    cmdList->Release();
    allocator->Release();
    queue->Release();
    rtResource->Release();
    rtvHeap->Release();
    device->Release();
    return true;
}

} // namespace

bool InstallCommandListProbe() {
    void* createRTVAddr = nullptr;
    void* executeAddr = nullptr;
    void* resetAddr = nullptr;
    void* closeAddr = nullptr;
    void* omSetRTAddr = nullptr;
    void* drawAddr = nullptr;
    void* drawIndexedAddr = nullptr;
    if (!GetRealAddresses(&createRTVAddr, &executeAddr, &resetAddr, &closeAddr,
                           &omSetRTAddr, &drawAddr, &drawIndexedAddr)) {
        return false;
    }

    struct HookSpec {
        void* target;
        void* detour;
        void** original;
        const char* name;
    };
    HookSpec hooks[] = {
        { createRTVAddr, &hkCreateRenderTargetView, reinterpret_cast<void**>(&oCreateRenderTargetView), "CreateRenderTargetView" },
        { executeAddr, &hkExecuteCommandLists, reinterpret_cast<void**>(&oExecuteCommandLists), "ExecuteCommandLists" },
        { resetAddr, &hkReset, reinterpret_cast<void**>(&oReset), "Reset" },
        { closeAddr, &hkClose, reinterpret_cast<void**>(&oClose), "Close" },
        { omSetRTAddr, &hkOMSetRenderTargets, reinterpret_cast<void**>(&oOMSetRenderTargets), "OMSetRenderTargets" },
        { drawAddr, &hkDrawInstanced, reinterpret_cast<void**>(&oDrawInstanced), "DrawInstanced" },
        { drawIndexedAddr, &hkDrawIndexedInstanced, reinterpret_cast<void**>(&oDrawIndexedInstanced), "DrawIndexedInstanced" },
    };

    // MH_Initialize() already called by InstallPresentHook() before this runs.
    for (const auto& h : hooks) {
        if (MH_CreateHook(h.target, h.detour, h.original) != MH_OK || MH_EnableHook(h.target) != MH_OK) {
            modlog::Line(std::string("CommandListProbe: ") + h.name + " hook setup failed.");
            return false;
        }
    }

    modlog::Line("CommandListProbe: installed. Press F10 in-game to capture several frames' "
                  "command-list structure to vrmod.log.");
    return true;
}

// Two consecutive single-frame captures (bounded by adjacent Present calls on
// whichever thread calls Present) came back with 0 lines despite
// ExecuteCommandLists firing ~1250/sec (confirmed via the heartbeat counters
// above) -- i.e. ~20 calls should fall in any given 16ms slice. That strongly
// suggests command-list recording happens on a render thread pipelined ahead
// of/decoupled from the main thread's Present cadence, so a single
// Present-to-Present slice doesn't reliably overlap it. Widened the window to
// span several consecutive frames so a capture can't land entirely in a gap.
constexpr int kCaptureFrameSpan = 10;
std::atomic<int> g_captureFramesRemaining{0};

void PollCommandListCaptureHotkey() {
    static bool wasDown = false;
    bool isDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    bool justPressed = isDown && !wasDown;
    wasDown = isDown;

    // This runs once per Present call, i.e. exactly at each frame boundary.
    if (g_captureActive.load(std::memory_order_relaxed)) {
        int remaining = g_captureFramesRemaining.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (remaining <= 0) {
            g_captureActive.store(false, std::memory_order_relaxed);

            std::vector<std::string> lines;
            {
                std::lock_guard<std::mutex> lock(g_captureLogMutex);
                lines.swap(g_captureLog);
            }
            modlog::Line("=== CAPTURE END (" + std::to_string(lines.size()) + " lines) ===");
            for (auto& l : lines) {
                modlog::Line(l);
            }

            std::lock_guard<std::mutex> lock(g_cmdListStateMutex);
            g_cmdListStates.clear();
        }
        return;
    }

    if (justPressed) {
        modlog::Line("=== CAPTURE BEGIN (F10 pressed), capturing next "
                      + std::to_string(kCaptureFrameSpan) + " frames ===");
        g_captureFramesRemaining.store(kCaptureFrameSpan, std::memory_order_relaxed);
        g_captureActive.store(true, std::memory_order_relaxed);
    }
}

} // namespace vrmod
