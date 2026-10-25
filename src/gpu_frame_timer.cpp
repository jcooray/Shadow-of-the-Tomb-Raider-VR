#include "gpu_frame_timer.h"

#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "logger.h"
#include "present_hook.h"
#include "perf_stats.h"
#include "settings.h"
#include "frame_log.h"
#include "ui_probe.h"

namespace vrmod {

namespace {

constexpr size_t kExecuteCommandListsIndex = 10;
constexpr int kReportEvery = 300;
constexpr uint32_t kRing = 1024;          // submission slots (each = a begin list + an end list)
constexpr uint32_t kMaxListsPerSubmit = 60;
constexpr uint32_t kDlssRing = 64;        // DLSS evaluates: 3 timestamps each (before our MV passes, before DLSS, after)

using ExecuteCommandLists_t = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteCommandLists_t g_origExecute = nullptr;
void* g_hookTarget = nullptr;

ID3D12CommandQueue* g_queue = nullptr;
ID3D12Fence* g_fence = nullptr;
ID3D12Device* g_device = nullptr;
ID3D12QueryHeap* g_heap = nullptr;
ID3D12Resource* g_readback = nullptr;
ID3D12CommandAllocator* g_allocator = nullptr;
ID3D12GraphicsCommandList* g_beginLists[kRing] = {};
ID3D12GraphicsCommandList* g_endLists[kRing] = {};
const uint64_t* g_results = nullptr;      // mapped readback: [slot*2] = begin, [slot*2+1] = end
double g_ticksPerMs = 0;

std::atomic<uint64_t> g_nextSlot{0};      // monotonically increasing submission counter
std::atomic<uint64_t> g_frameStartSlot{0};
std::atomic<uint8_t> g_slotOurs[kRing] = {};   // 1 = our own submit (the eye copy), 0 = the game's

ID3D12QueryHeap* g_dlssHeap = nullptr;
ID3D12Resource* g_dlssReadback = nullptr;
const uint64_t* g_dlssResults = nullptr;       // [slot*3 + point]
std::atomic<uint64_t> g_dlssNext{0};
std::atomic<uint64_t> g_dlssFrameStart{0};
thread_local uint64_t tl_dlssSlot = 0;
std::atomic<bool> g_inOurSubmit{false};
std::atomic<bool> g_run{false};
std::atomic<bool> g_measuring{false};

struct Pending { uint64_t fence; uint64_t slotBegin; uint64_t slotEnd; uint64_t dlssBegin; uint64_t dlssEnd; uint64_t presentFrame; };
std::mutex g_mutex;
std::deque<Pending> g_pending;
HANDLE g_semaphore = nullptr;
HANDLE g_event = nullptr;
std::thread g_thread;

// Brackets each submit on the game's queue with a begin and an end timestamp (ours included, marked as ours).
bool WrapExecute(ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists, UiProbeExecuteFn original) {
    if (self == g_queue && g_measuring.load(std::memory_order_relaxed) && n > 0 && n <= kMaxListsPerSubmit) {
        const uint64_t s = g_nextSlot.fetch_add(1);
        const uint32_t ring = static_cast<uint32_t>(s % kRing);
        g_slotOurs[ring].store(g_inOurSubmit.load(std::memory_order_relaxed) ? 1 : 0, std::memory_order_relaxed);
        ID3D12CommandList* arr[kMaxListsPerSubmit + 2];
        arr[0] = g_beginLists[ring];
        memcpy(arr + 1, lists, n * sizeof(ID3D12CommandList*));
        arr[n + 1] = g_endLists[ring];
        original(self, n + 2, arr);
        return true;
    }
    return false;
}

void __stdcall hkExecute(ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists) {
    if (!WrapExecute(self, n, lists, g_origExecute)) g_origExecute(self, n, lists);
}

bool CreateTimestampObjects() {
    if (FAILED(g_queue->GetDevice(IID_PPV_ARGS(&g_device)))) return false;
    UINT64 freq = 0;
    if (FAILED(g_queue->GetTimestampFrequency(&freq)) || freq == 0) return false;
    g_ticksPerMs = static_cast<double>(freq) / 1000.0;

    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = kRing * 2;
    if (FAILED(g_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_heap)))) return false;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = static_cast<UINT64>(kRing) * 2 * sizeof(uint64_t);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&g_readback)))) return false;
    void* mapped = nullptr;
    D3D12_RANGE readAll{ 0, static_cast<SIZE_T>(rd.Width) };
    if (FAILED(g_readback->Map(0, &readAll, &mapped))) return false;
    g_results = static_cast<const uint64_t*>(mapped);

    qh.Count = kDlssRing * 3;
    if (FAILED(g_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_dlssHeap)))) return false;
    rd.Width = static_cast<UINT64>(kDlssRing) * 3 * sizeof(uint64_t);
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&g_dlssReadback)))) return false;
    D3D12_RANGE readDlss{ 0, static_cast<SIZE_T>(rd.Width) };
    if (FAILED(g_dlssReadback->Map(0, &readDlss, &mapped))) return false;
    g_dlssResults = static_cast<const uint64_t*>(mapped);

    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator)))) return false;
    for (uint32_t i = 0; i < kRing; ++i) {
        for (int pass = 0; pass < 2; ++pass) {
            ID3D12GraphicsCommandList* cl = nullptr;
            if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocator, nullptr, IID_PPV_ARGS(&cl)))) return false;
            if (pass == 0) {
                cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
                g_beginLists[i] = cl;
            } else {
                cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
                cl->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, i * 2, 2, g_readback, static_cast<UINT64>(i) * 16);
                g_endLists[i] = cl;
            }
            cl->Close();
        }
    }
    return true;
}

void DestroyTimestampObjects() {
    for (auto& l : g_beginLists) { if (l) { l->Release(); l = nullptr; } }
    for (auto& l : g_endLists) { if (l) { l->Release(); l = nullptr; } }
    if (g_allocator) { g_allocator->Release(); g_allocator = nullptr; }
    if (g_readback) { g_readback->Unmap(0, nullptr); g_readback->Release(); g_readback = nullptr; }
    g_results = nullptr;
    if (g_heap) { g_heap->Release(); g_heap = nullptr; }
    if (g_dlssReadback) { g_dlssReadback->Unmap(0, nullptr); g_dlssReadback->Release(); g_dlssReadback = nullptr; }
    g_dlssResults = nullptr;
    if (g_dlssHeap) { g_dlssHeap->Release(); g_dlssHeap = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

double Percentile(std::vector<double>& sorted, double p) {
    return sorted[static_cast<size_t>((sorted.size() - 1) * p)];
}

// One frame's GPU time by stage (ms). The game's DLSS evaluate sits inside one of its submits; its timestamps split
// the game's time into before and after.
struct Stages { double pre = 0, mv = 0, dlss = 0, post = 0, ours = 0, busy = 0, interval = 0; bool hasDlss = false; };

void Report(std::vector<Stages>& frames) {
    Stages sum;
    std::vector<double> busy;
    int withDlss = 0;
    for (const Stages& f : frames) {
        sum.pre += f.pre; sum.mv += f.mv; sum.dlss += f.dlss; sum.post += f.post; sum.ours += f.ours;
        sum.busy += f.busy; sum.interval += f.interval;
        busy.push_back(f.busy);
        withDlss += f.hasDlss ? 1 : 0;
    }
    const double n = static_cast<double>(frames.size());
    std::sort(busy.begin(), busy.end());
    const double interval = sum.interval / n, total = sum.busy / n;
    const double idle = interval > total ? interval - total : 0.0;
    std::ostringstream o;
    o << std::fixed << std::setprecision(2)
      << "GPUSTAGES (per " << frames.size() << " frames, avg ms): game before DLSS " << sum.pre / n
      << " | our MV passes " << sum.mv / n << " | DLSS incl. Cheeky/NR " << sum.dlss / n
      << " | game after DLSS " << sum.post / n << " | our eye copy " << sum.ours / n
      << " || GPU busy " << total << " (p50 " << Percentile(busy, 0.5) << ", p95 " << Percentile(busy, 0.95)
      << ", max " << busy.back() << ") of frame interval " << interval << " -> idle " << idle << " ("
      << std::setprecision(0) << (interval > 0 ? 100.0 * idle / interval : 0.0) << "%) | frames with a DLSS evaluate "
      << withDlss;
    modlog::Line(o.str());
}

double Ms(uint64_t ticks) { return static_cast<double>(ticks) / g_ticksPerMs; }

void WorkerMain() {
    double prevComplete = 0;
    std::vector<Stages> frames;
    while (g_run.load()) {
        if (WaitForSingleObject(g_semaphore, 200) != WAIT_OBJECT_0) continue;
        Pending p;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_pending.empty()) continue;
            p = g_pending.front();
            g_pending.pop_front();
        }
        if (g_fence->GetCompletedValue() < p.fence) {
            g_fence->SetEventOnCompletion(p.fence, g_event);
            while (g_run.load() && WaitForSingleObject(g_event, 200) != WAIT_OBJECT_0) {}
            if (!g_run.load()) break;
        }
        const double complete = NowMs();
        const uint64_t subs = p.slotEnd - p.slotBegin;
        // Only trust frames whose slots can't have been overwritten by the ring yet.
        if (prevComplete > 0 && subs > 0 && subs < kRing / 2) {
            Stages f;
            uint64_t spanBegin = UINT64_MAX, spanEnd = 0, gameTicks = 0;
            for (uint64_t s = p.slotBegin; s < p.slotEnd; ++s) {
                const uint32_t r = static_cast<uint32_t>(s % kRing);
                const uint64_t b = g_results[r * 2], e = g_results[r * 2 + 1];
                if (e <= b) continue;
                if (g_slotOurs[r].load(std::memory_order_relaxed)) { f.ours += Ms(e - b); continue; }
                gameTicks += e - b;
                spanBegin = (std::min)(spanBegin, b);
                spanEnd = (std::max)(spanEnd, e);
            }
            // This frame's DLSS evaluate: the one whose timestamps fall inside the game's submits of the frame (the
            // render thread may run a little ahead of Present, so look one either side).
            uint64_t t0 = 0, t1 = 0, t2 = 0;
            const uint64_t lo = p.dlssBegin > 0 ? p.dlssBegin - 1 : 0;
            for (uint64_t d = lo; d <= p.dlssEnd && g_dlssResults; ++d) {
                const uint32_t r = static_cast<uint32_t>(d % kDlssRing);
                const uint64_t a = g_dlssResults[r * 3], b = g_dlssResults[r * 3 + 1], c = g_dlssResults[r * 3 + 2];
                if (a >= spanBegin && c <= spanEnd && a <= b && b <= c) { t0 = a; t1 = b; t2 = c; f.hasDlss = true; break; }
            }
            const double game = Ms(gameTicks);
            if (f.hasDlss) {
                // The game's time before the evaluate: its submits' busy time up to t0.
                uint64_t pre = 0;
                for (uint64_t s = p.slotBegin; s < p.slotEnd; ++s) {
                    const uint32_t r = static_cast<uint32_t>(s % kRing);
                    const uint64_t b = g_results[r * 2], e = g_results[r * 2 + 1];
                    if (e <= b || g_slotOurs[r].load(std::memory_order_relaxed)) continue;
                    if (b < t0) pre += (std::min)(e, t0) - b;
                }
                f.pre = Ms(pre);
                f.mv = Ms(t1 - t0);
                f.dlss = Ms(t2 - t1);
                f.post = game - f.pre - f.mv - f.dlss;
                if (f.post < 0) f.post = 0;
            } else {
                f.pre = game;
            }
            f.busy = game + f.ours;
            f.interval = complete - prevComplete;
            FrameLogSetGpuMs(p.presentFrame, static_cast<float>(f.busy));
            frames.push_back(f);
            if (static_cast<int>(frames.size()) >= kReportEvery) {
                if (GetLogSwitches().perf) Report(frames);
                frames.clear();
            }
        }
        prevComplete = complete;
    }
}

} // namespace

bool GpuFrameTimerStart(ID3D12CommandQueue* gameQueue, ID3D12Fence* fence) {
    if (g_run.load()) return true;
    if (!gameQueue || !fence) return false;
    if (!EnsureMinHookInitialized()) return false;
    g_queue = gameQueue;
    g_fence = fence;
    if (!CreateTimestampObjects()) {
        modlog::Line("GpuFrameTimer: couldn't create the timestamp query objects; GPU frame timing off.");
        DestroyTimestampObjects();
        return false;
    }
    void** vtbl = *reinterpret_cast<void***>(gameQueue);
    g_hookTarget = vtbl[kExecuteCommandListsIndex];
    const MH_STATUS st = MH_CreateHook(g_hookTarget, &hkExecute, reinterpret_cast<void**>(&g_origExecute));
    if (st == MH_OK && MH_EnableHook(g_hookTarget) == MH_OK) {
        // hooked directly
    } else if (st == MH_ERROR_ALREADY_CREATED && UiProbeSetExecuteFilter(&WrapExecute)) {
        // The HUD hooks already own ExecuteCommandLists: ride on theirs.
        g_hookTarget = nullptr;
        modlog::Line("GpuFrameTimer: ExecuteCommandLists is hooked by the HUD code; timing through its hook.");
    } else {
        modlog::Line("GpuFrameTimer: ExecuteCommandLists hook failed (MinHook " + std::to_string(static_cast<int>(st)) +
                     "); GPU frame timing off.");
        g_hookTarget = nullptr;
        DestroyTimestampObjects();
        return false;
    }
    g_semaphore = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_run.store(true);
    g_measuring.store(true);
    g_thread = std::thread(WorkerMain);
    modlog::Line("GpuFrameTimer: started with GPU timestamp queries (GPUSTAGES every 300 frames with the perf switch on).");
    return true;
}

void GpuFrameTimerStop() {
    if (!g_run.exchange(false)) return;
    g_measuring.store(false);
    if (g_thread.joinable()) g_thread.join();
    UiProbeSetExecuteFilter(nullptr);
    if (g_hookTarget) {
        MH_DisableHook(g_hookTarget);
        MH_RemoveHook(g_hookTarget);
        g_hookTarget = nullptr;
    }
    if (g_semaphore) { CloseHandle(g_semaphore); g_semaphore = nullptr; }
    if (g_event) { CloseHandle(g_event); g_event = nullptr; }
    // The timestamp lists may still be queued on the GPU; the caller waits for the queue's last fence value first.
    DestroyTimestampObjects();
    g_queue = nullptr;
    g_fence = nullptr;
}

void GpuFrameTimerOurSubmit(bool begin) {
    g_inOurSubmit.store(begin, std::memory_order_relaxed);
}

void GpuFrameTimerFrameSubmitted(uint64_t fenceValue, uint64_t presentFrame) {
    if (!g_run.load()) return;
    const uint64_t end = g_nextSlot.load();
    const uint64_t begin = g_frameStartSlot.exchange(end);
    const uint64_t dEnd = g_dlssNext.load();
    const uint64_t dBegin = g_dlssFrameStart.exchange(dEnd);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.push_back({ fenceValue, begin, end, dBegin, dEnd, presentFrame });
    }
    ReleaseSemaphore(g_semaphore, 1, nullptr);
}

void GpuFrameTimerDlssMark(ID3D12GraphicsCommandList* list, int point) {
    if (!g_run.load(std::memory_order_relaxed) || !list || !g_dlssHeap) return;
    if (point == 0) tl_dlssSlot = g_dlssNext.fetch_add(1);
    const uint32_t r = static_cast<uint32_t>(tl_dlssSlot % kDlssRing);
    list->EndQuery(g_dlssHeap, D3D12_QUERY_TYPE_TIMESTAMP, r * 3 + static_cast<UINT>(point));
    if (point == 2)
        list->ResolveQueryData(g_dlssHeap, D3D12_QUERY_TYPE_TIMESTAMP, r * 3, 3, g_dlssReadback,
                               static_cast<UINT64>(r) * 3 * sizeof(uint64_t));
}

}
