#include "d3d12_debug.h"

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "frame_log.h"
#include "logger.h"
#include "settings.h"

namespace vrmod {

namespace {

bool g_layerEnabled = false;

const char* SeverityName(D3D12_MESSAGE_SEVERITY s) {
    switch (s) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
    case D3D12_MESSAGE_SEVERITY_ERROR: return "ERROR";
    case D3D12_MESSAGE_SEVERITY_WARNING: return "WARNING";
    case D3D12_MESSAGE_SEVERITY_INFO: return "INFO";
    case D3D12_MESSAGE_SEVERITY_MESSAGE: return "MESSAGE";
    }
    return "?";
}

} // namespace

void D3D12DebugEnableIfRequested() {
    if (!GetDebugSwitches().d3d12DebugLayer) {
        return;
    }
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto getDebug = d3d12 ? reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
    ID3D12Debug* debug = nullptr;
    const HRESULT hr = getDebug ? getDebug(IID_PPV_ARGS(&debug)) : E_FAIL;
    if (FAILED(hr) || !debug) {
        std::ostringstream o;
        o << "D3D12Debug: debug layer requested but unavailable (hr=0x" << std::hex << static_cast<unsigned>(hr)
          << "). Install the Windows optional feature 'Graphics Tools' to get d3d12SDKLayers.dll.";
        modlog::Line(o.str());
        return;
    }
    debug->EnableDebugLayer();
    debug->Release();
    g_layerEnabled = true;
    modlog::Line("D3D12Debug: debug layer ENABLED (expect a large slowdown; test at low resolution).");
}

void D3D12DebugDrain(ID3D12Device* device) {
    static ID3D12InfoQueue* queue = nullptr;
    static bool tried = false;
    static std::map<int, uint64_t> perId;
    static uint64_t total = 0, errors = 0, warnings = 0;
    static uint64_t drains = 0;

    if (!g_layerEnabled || !device) {
        return;
    }
    if (!tried) {
        tried = true;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {
            queue = nullptr;
            modlog::Line("D3D12Debug: no ID3D12InfoQueue on the game's device (was the layer enabled before the device was created?).");
            return;
        }
        D3D12_MESSAGE_SEVERITY deny[] = { D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = static_cast<UINT>(sizeof(deny) / sizeof(deny[0]));
        filter.DenyList.pSeverityList = deny;
        queue->PushStorageFilter(&filter);
        modlog::Line("D3D12Debug: info queue attached; logging warnings and errors (first 3 of each message ID).");
    }
    if (!queue) {
        return;
    }

    const UINT64 n = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    std::vector<char> buf;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &len)) || len == 0) continue;
        buf.resize(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(queue->GetMessage(i, msg, &len))) continue;
        ++total;
        if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++errors; else if (msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++warnings;
        const uint64_t seen = ++perId[static_cast<int>(msg->ID)];
        if (seen <= 3) {
            std::ostringstream o;
            o << "D3D12DEBUG " << SeverityName(msg->Severity) << " id=" << static_cast<int>(msg->ID)
              << " frame=" << FrameLogCurrentFrame() << ": " << (msg->pDescription ? msg->pDescription : "");
            modlog::Line(o.str());
        }
    }
    if (n > 0) {
        queue->ClearStoredMessages();
        FrameLogAddDebugMessages(static_cast<uint32_t>(n));
    }
    if (++drains % 1800 == 0 && total > 0) {
        modlog::Line("D3D12Debug: " + std::to_string(total) + " messages so far (" + std::to_string(errors)
            + " errors/corruption, " + std::to_string(warnings) + " warnings, " + std::to_string(perId.size()) + " distinct IDs).");
    }
}

}
