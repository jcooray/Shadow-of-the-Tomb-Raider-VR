#include "face_track.h"

#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include "logger.h"

// Head lock from the face draw (2026-10-07). The skeleton search used to start at the offset camera, which hangs off the
// third-person boom: right after a checkpoint load it jumped 2.8 m between two places, and Lara's spawn pose failed the
// standing/crouching checks, so the lock took ~10 s. The face draw knows exactly where her head is. From the RenderDoc
// capture (frame 10430, event 11280), its vertex shader reads, through root parameter 2 -- a 14-CBV table b0..b13 in the
// shader-visible heap:
//   b0  WorldBuffer   -- PerInstance[0].World at byte 128 (row-major 4x4, translation in row 3): the model's root;
//   b1  CB_Skinning   -- float3x4 skinning matrices (rows: r00 r01 r02 tx / r10 r11 r12 ty / r20 r21 r22 tz), world space;
//   b11 ModelDrawable -- the face's model-space bounds (-9.0 -10.8 108.1)..(9.0 10.0 149.9), 17 bones.
// The CBVs are written into descriptor heaps (CreateConstantBufferView, CopyDescriptors[Simple]) and the data lives in
// a GPU-only (default heap) buffer, filled once a frame by one CopyBufferRegion from a mapped upload buffer (capture
// event 10873: Buffer 493142 -> Buffer 12653, 1 MB). So the mod follows descriptor writes per heap slot, the root
// tables per command list, every Map of a buffer and every buffer copy out of a mapped one; at the face draw it resolves
// b0 and b1 and reads them when the copy that fills them is recorded -- the game records that copy AFTER the frame's
// draws (2026-10-07 test: resolving through the newest earlier copy read the previous frame's, already reused, upload
// memory -- 2-3 % plausible reads, some garbage, false "lock lost" re-locates and frame drops).
// Vtable slots from the Windows 10.0.26100.0 SDK d3d12.h: ID3D12Device CreateDescriptorHeap 14,
// CreateConstantBufferView 17, CopyDescriptors 23, CopyDescriptorsSimple 24; ID3D12GraphicsCommandList
// CopyBufferRegion 15, SetGraphicsRootDescriptorTable 32; ID3D12Resource Map 8.

namespace vrmod {
namespace {

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

constexpr UINT kFaceTableParam = 2;    // root parameter: the VS CBV table
constexpr UINT kWorldSlot = 0;         // b0 WorldBuffer
constexpr UINT kSkinSlot = 1;          // b1 CB_Skinning
const float kFaceCentre[3] = { 0.0f, -0.43f, 128.97f };   // centre of the face's model-space bounds (capture)

struct Cbv { uint64_t va; uint32_t size; };
struct HeapRec {
    SIZE_T cpuStart = 0;
    uint64_t gpuStart = 0;   // 0 unless shader-visible
    UINT count = 0, inc = 0;
    std::unique_ptr<Cbv[]> e;
};
std::shared_mutex g_heapMutex;
std::vector<std::unique_ptr<HeapRec>> g_heaps;   // sorted by cpuStart
UINT g_inc = 0;

struct MapRec { uint64_t va; uint64_t size; uint8_t* cpu; };
std::mutex g_mapMutex;
std::unordered_map<const void*, MapRec> g_maps;   // mapped buffers by resource
// Face draws waiting for the copy that fills their constants (guarded by g_mapMutex).
struct Pending { uint64_t world, skin; double ms; };
constexpr int kPending = 4;
Pending g_pending[kPending];
std::atomic<uint64_t> g_statCopyBuf{ 0 };

bool ReadGuarded(const void* p, void* out, size_t n);
void Evaluate(const uint8_t* worldCpu, const uint8_t* skinCpu);
void BodyLearnCopy(uint64_t lo, uint64_t hi, const uint8_t* cpu);
bool BodyLearnWanted();

std::atomic<uint64_t> g_statCbv{ 0 }, g_statCopies{ 0 }, g_statFace{ 0 }, g_statOk{ 0 }, g_statPending{ 0 };
std::atomic<int> g_lastFail{ 0 };
const char* const kFailName[] = { "-", "no table bound", "table not in a tracked heap", "b0/b1 not a known CBV",
                                  "CBV data not mapped", "unreadable", "not a rotation", "skin and World disagree",
                                  "jumped from the previous reading" };

std::atomic<float> g_head[3];
std::atomic<double> g_headMs{ 0.0 };
std::atomic<float> g_rootYaw[2];      // yaw (world, as the camera's: atan2(-x, y)) of the model's local X and Y axes
std::atomic<float> g_rootPos[3];
std::atomic<double> g_rootMs{ 0.0 };
// Body learning (see below): the learning frame's face World, taken by Evaluate.
float g_learnFaceW[16];
std::atomic<bool> g_learnFaceOk{ false };   // g_learnFaceW holds the learning frame's face World
std::atomic<bool> g_learnFaceOpen{ false };  // waiting for that reading

HeapRec* FindCpuLocked(SIZE_T p) {
    auto it = std::upper_bound(g_heaps.begin(), g_heaps.end(), p, [](SIZE_T v, const std::unique_ptr<HeapRec>& h) { return v < h->cpuStart; });
    if (it == g_heaps.begin()) return nullptr;
    HeapRec* h = (it - 1)->get();
    return p < h->cpuStart + static_cast<SIZE_T>(h->count) * h->inc ? h : nullptr;
}

HeapRec* FindGpuLocked(uint64_t p) {
    for (const auto& h : g_heaps) {
        if (h->gpuStart && p >= h->gpuStart && p < h->gpuStart + static_cast<uint64_t>(h->count) * h->inc) return h.get();
    }
    return nullptr;
}

// ---- device hooks ----
using CreateHeap_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using CreateCbv_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDesc_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopySimple_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using SetTable_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using Map_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
using CopyBuf_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
CreateHeap_t oCreateHeap = nullptr;
CreateCbv_t oCreateCbv = nullptr;
CopyDesc_t oCopyDesc = nullptr;
CopySimple_t oCopySimple = nullptr;
SetTable_t oSetTable = nullptr;
Map_t oMap = nullptr;
CopyBuf_t oCopyBuf = nullptr;

HRESULT STDMETHODCALLTYPE hkCreateHeap(ID3D12Device* dev, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID riid, void** out) {
    const HRESULT hr = oCreateHeap(dev, d, riid, out);
    if (FAILED(hr) || !d || !out || !*out || d->Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return hr;
    ID3D12DescriptorHeap* heap = nullptr;
    if (FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&heap))) || !heap) return hr;
    auto rec = std::make_unique<HeapRec>();
    rec->cpuStart = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (d->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) rec->gpuStart = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    heap->Release();
    rec->count = d->NumDescriptors;
    rec->inc = g_inc;
    rec->e.reset(new (std::nothrow) Cbv[d->NumDescriptors]());
    if (!rec->e) return hr;
    std::unique_lock<std::shared_mutex> lock(g_heapMutex);
    // A new heap at an address range an old (released) one used replaces it.
    const SIZE_T lo = rec->cpuStart, hi = rec->cpuStart + static_cast<SIZE_T>(rec->count) * rec->inc;
    g_heaps.erase(std::remove_if(g_heaps.begin(), g_heaps.end(), [&](const std::unique_ptr<HeapRec>& h) {
        return h->cpuStart < hi && h->cpuStart + static_cast<SIZE_T>(h->count) * h->inc > lo;
    }), g_heaps.end());
    auto it = std::upper_bound(g_heaps.begin(), g_heaps.end(), lo, [](SIZE_T v, const std::unique_ptr<HeapRec>& h) { return v < h->cpuStart; });
    g_heaps.insert(it, std::move(rec));
    return hr;
}

void STDMETHODCALLTYPE hkCreateCbv(ID3D12Device* dev, const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    oCreateCbv(dev, d, h);
    std::shared_lock<std::shared_mutex> lock(g_heapMutex);
    if (HeapRec* r = FindCpuLocked(h.ptr)) {
        r->e[(h.ptr - r->cpuStart) / r->inc] = d ? Cbv{ d->BufferLocation, d->SizeInBytes } : Cbv{ 0, 0 };
        g_statCbv.fetch_add(1, std::memory_order_relaxed);
    }
}

// Copies n slot records from src to dst (both CBV_SRV_UAV); untracked sources clear the destination.
void CopyLocked(SIZE_T dst, SIZE_T src, UINT n) {
    HeapRec* d = FindCpuLocked(dst);
    if (!d) return;
    UINT di = static_cast<UINT>((dst - d->cpuStart) / d->inc);
    n = (std::min)(n, d->count - di);
    HeapRec* s = FindCpuLocked(src);
    if (!s) { std::fill(d->e.get() + di, d->e.get() + di + n, Cbv{ 0, 0 }); return; }
    const UINT si = static_cast<UINT>((src - s->cpuStart) / s->inc);
    n = (std::min)(n, s->count - si);
    std::memmove(d->e.get() + di, s->e.get() + si, sizeof(Cbv) * n);
    g_statCopies.fetch_add(n, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE hkCopySimple(ID3D12Device* dev, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                    D3D12_DESCRIPTOR_HEAP_TYPE type) {
    oCopySimple(dev, n, dst, src, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return;
    std::shared_lock<std::shared_mutex> lock(g_heapMutex);
    CopyLocked(dst.ptr, src.ptr, n);
}

void STDMETHODCALLTYPE hkCopyDesc(ID3D12Device* dev, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    oCopyDesc(dev, nd, ds, dsz, ns, ss, ssz, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !ds || !ss || !g_inc) return;
    std::shared_lock<std::shared_mutex> lock(g_heapMutex);
    // Walk both range lists in step (a null size array means ranges of one descriptor).
    UINT di = 0, si = 0, doff = 0, soff = 0;
    while (di < nd && si < ns) {
        const UINT dn = dsz ? dsz[di] : 1, sn = ssz ? ssz[si] : 1;
        const UINT n = (std::min)(dn - doff, sn - soff);
        CopyLocked(ds[di].ptr + static_cast<SIZE_T>(doff) * g_inc, ss[si].ptr + static_cast<SIZE_T>(soff) * g_inc, n);
        doff += n; soff += n;
        if (doff >= dn) { ++di; doff = 0; }
        if (soff >= sn) { ++si; soff = 0; }
    }
}

HRESULT STDMETHODCALLTYPE hkMap(ID3D12Resource* res, UINT sub, const D3D12_RANGE* range, void** data) {
    const HRESULT hr = oMap(res, sub, range, data);
    if (FAILED(hr) || !data || !*data || sub != 0) return hr;
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return hr;
    const uint64_t va = res->GetGPUVirtualAddress();
    if (!va) return hr;
    std::lock_guard<std::mutex> lock(g_mapMutex);
    if (g_maps.size() < 16384 || g_maps.count(res)) g_maps[res] = { va, rd.Width, static_cast<uint8_t*>(*data) };
    return hr;
}

void STDMETHODCALLTYPE hkCopyBuf(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT64 dstOff, ID3D12Resource* src, UINT64 srcOff, UINT64 n) {
    oCopyBuf(cl, dst, dstOff, src, srcOff, n);
    if (!dst || !src || n < 256) return;
    const uint8_t* wp = nullptr;
    const uint8_t* sp = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        bool any = false;
        for (const Pending& q : g_pending) any |= q.world != 0;
        if (!any && !BodyLearnWanted()) return;
        auto it = g_maps.find(src);
        if (it == g_maps.end() || srcOff + n > it->second.size) return;
        const uint64_t lo = dst->GetGPUVirtualAddress() + dstOff, hi = lo + n;
        const uint8_t* cpu = it->second.cpu + srcOff;
        const double now = NowMs();
        for (Pending& q : g_pending) {
            if (!q.world) continue;
            if (now - q.ms > 200.0) { q = Pending{}; continue; }
            if (q.world >= lo && q.world + 192 <= hi && q.skin >= lo && q.skin + 48 <= hi) {
                wp = cpu + (q.world - lo);
                sp = cpu + (q.skin - lo);
                q = Pending{};
                break;
            }
        }
        g_statCopyBuf.fetch_add(1, std::memory_order_relaxed);
    }
    if (wp) Evaluate(wp, sp);
    // Body learning: the Worlds this copy fills (draws of the learning frame).
    {
        const uint8_t* cpu = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            auto it = g_maps.find(src);
            if (it != g_maps.end() && srcOff + n <= it->second.size) cpu = it->second.cpu + srcOff;
        }
        if (cpu) BodyLearnCopy(dst->GetGPUVirtualAddress() + dstOff, dst->GetGPUVirtualAddress() + dstOff + n, cpu);
    }
}

// Per command list: the root descriptor table bound at kFaceTableParam (recording is per thread).
struct ListTables { const void* list = nullptr; uint64_t table = 0; };
thread_local ListTables t_lists[8];
thread_local int t_listNext = 0;
ListTables& Lt(const void* cl) {
    for (ListTables& t : t_lists) if (t.list == cl) return t;
    ListTables& t = t_lists[t_listNext];
    t_listNext = (t_listNext + 1) % 8;
    t = ListTables{};
    t.list = cl;
    return t;
}

void STDMETHODCALLTYPE hkSetTable(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (index == kFaceTableParam) Lt(cl).table = h.ptr;
    oSetTable(cl, index, h);
}

bool ReadGuarded(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// CPU memory of a mapped buffer covering va (constants read straight from an upload buffer).
const uint8_t* CpuFor(uint64_t va, uint64_t need) {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    for (const auto& kv : g_maps) {
        const MapRec& m = kv.second;
        if (va >= m.va && va + need <= m.va + m.size) return m.cpu + (va - m.va);
    }
    return nullptr;
}

bool RowUnit(const float* r) {
    const float l = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
    return std::isfinite(l) && l > 0.8f && l < 1.2f;
}

bool Hook(void* target, void* detour, void** orig, const char* name) {
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        modlog::Line(std::string("FACETRACK: hook failed: ") + name);
        return false;
    }
    return true;
}

}  // namespace

void FaceTrackInstall(ID3D12Device* dev, void** dv, void** lv) {
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    bool ok = Hook(dv[14], reinterpret_cast<void*>(&hkCreateHeap), reinterpret_cast<void**>(&oCreateHeap), "CreateDescriptorHeap")
           && Hook(dv[17], reinterpret_cast<void*>(&hkCreateCbv), reinterpret_cast<void**>(&oCreateCbv), "CreateConstantBufferView")
           && Hook(dv[23], reinterpret_cast<void*>(&hkCopyDesc), reinterpret_cast<void**>(&oCopyDesc), "CopyDescriptors")
           && Hook(dv[24], reinterpret_cast<void*>(&hkCopySimple), reinterpret_cast<void**>(&oCopySimple), "CopyDescriptorsSimple");
    if (lv) ok = Hook(lv[32], reinterpret_cast<void*>(&hkSetTable), reinterpret_cast<void**>(&oSetTable), "SetGraphicsRootDescriptorTable") && ok;
    if (lv) ok = Hook(lv[15], reinterpret_cast<void*>(&hkCopyBuf), reinterpret_cast<void**>(&oCopyBuf), "CopyBufferRegion") && ok;
    // ID3D12Resource::Map, from a throwaway upload buffer (all buffers share the vtable).
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf))) && buf) {
        void** rv = *reinterpret_cast<void***>(buf);
        ok = Hook(rv[8], reinterpret_cast<void*>(&hkMap), reinterpret_cast<void**>(&oMap), "ID3D12Resource::Map") && ok;
        buf->Release();
    } else {
        ok = false;
    }
    modlog::Line(std::string("FACETRACK: ") + (ok ? "descriptor/Map hooks installed (head lock from the face draw)" : "hooks incomplete -- head lock uses the offset camera only"));
}

void FaceTrackForgetList(const void* list) { Lt(list).table = 0; }

void FaceTrackOnFaceDraw(const void* list) {
    g_statFace.fetch_add(1, std::memory_order_relaxed);
    auto fail = [](int why) { g_lastFail.store(why, std::memory_order_relaxed); };
    const uint64_t table = Lt(list).table;
    if (!table) return fail(1);
    Cbv world{}, skin{};
    {
        std::shared_lock<std::shared_mutex> lock(g_heapMutex);
        HeapRec* h = FindGpuLocked(table);
        if (!h) return fail(2);
        const UINT i = static_cast<UINT>((table - h->gpuStart) / h->inc);
        if (i + kSkinSlot >= h->count) return fail(2);
        world = h->e[i + kWorldSlot];
        skin = h->e[i + kSkinSlot];
    }
    if (!world.va || !skin.va || world.size < 192 || skin.size < 48) return fail(3);
    const uint8_t* wp = CpuFor(world.va, 192);
    const uint8_t* sp = CpuFor(skin.va, 48);
    if (wp && sp) return Evaluate(wp, sp);
    // Not mapped: read when the copy that fills them is recorded (hkCopyBuf).
    std::lock_guard<std::mutex> lock(g_mapMutex);
    Pending* slot = &g_pending[0];
    for (Pending& q : g_pending) {
        if (q.world == world.va) { slot = &q; break; }
        if (q.ms < slot->ms) slot = &q;
    }
    *slot = { world.va, skin.va, NowMs() };
    g_statPending.fetch_add(1, std::memory_order_relaxed);
}

namespace {
void Evaluate(const uint8_t* wp, const uint8_t* sp) {
    auto fail = [](int why) { g_lastFail.store(why, std::memory_order_relaxed); };
    float w[16], s[12];
    if (!ReadGuarded(wp + 128, w, sizeof(w)) || !ReadGuarded(sp, s, sizeof(s))) return fail(5);
    // Skin matrix rows (r00 r01 r02 tx), (r10 r11 r12 ty), (r20 r21 r22 tz); World row-major with translation in row 3.
    const float c0[3] = { s[0], s[4], s[8] }, c1[3] = { s[1], s[5], s[9] }, c2[3] = { s[2], s[6], s[10] };
    if (!RowUnit(s) || !RowUnit(s + 4) || !RowUnit(s + 8) || !RowUnit(c0) || !RowUnit(c1) || !RowUnit(c2) || !RowUnit(w) || !RowUnit(w + 4)
        || !RowUnit(w + 8) || std::fabs(w[15] - 1.0f) > 1e-3f)
        return fail(6);
    float p[3];
    for (int r = 0; r < 3; ++r) {
        p[r] = s[r * 4] * kFaceCentre[0] + s[r * 4 + 1] * kFaceCentre[1] + s[r * 4 + 2] * kFaceCentre[2] + s[r * 4 + 3];
    }
    // Her head is within 2.5 m of her model's root, whatever the pose.
    const float dx = p[0] - w[12], dy = p[1] - w[13], dz = p[2] - w[14];
    if (!std::isfinite(p[0] + p[1] + p[2]) || dx * dx + dy * dy + dz * dz > 250.0f * 250.0f) return fail(7);
    // Published only when the previous reading (under 300 ms old) agrees within 50 cm: one stray read can't move it.
    static float prev[3] = { 0, 0, 0 };
    static double prevMs = 0.0;
    const double now = NowMs();
    const float ex = p[0] - prev[0], ey = p[1] - prev[1], ez = p[2] - prev[2];
    const bool agrees = now - prevMs < 300.0 && ex * ex + ey * ey + ez * ez < 50.0f * 50.0f;
    for (int k = 0; k < 3; ++k) prev[k] = p[k];
    prevMs = now;
    if (!agrees) return fail(8);
    for (int k = 0; k < 3; ++k) g_head[k].store(p[k], std::memory_order_relaxed);
    g_headMs.store(now, std::memory_order_release);
    // The model's root (World rows: local X, Y, Z axes in world; row 3 the position): which way her body faces (the look
    // limit, head_drive.cpp), as the yaw of local X and local Y.
    g_rootYaw[0].store(std::atan2(-w[0], w[1]), std::memory_order_relaxed);
    g_rootYaw[1].store(std::atan2(-w[4], w[5]), std::memory_order_relaxed);
    for (int k = 0; k < 3; ++k) g_rootPos[k].store(w[12 + k], std::memory_order_relaxed);
    g_rootMs.store(now, std::memory_order_release);
    if (g_learnFaceOpen.exchange(false)) {   // body learning: this frame's face World
        memcpy(g_learnFaceW, w, sizeof w);
        g_learnFaceOk.store(true);
    }
    g_statOk.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

bool FaceHeadPoint(float out[3], double maxAgeMs) {
    const double t = g_headMs.load(std::memory_order_acquire);
    if (t <= 0.0 || NowMs() - t > maxAgeMs) return false;
    for (int k = 0; k < 3; ++k) out[k] = g_head[k].load(std::memory_order_relaxed);
    return true;
}

bool FaceTrackDrawKeys(const void* list, uint64_t* table, uint64_t* b0, uint64_t* b1) {
    *table = Lt(list).table;
    *b0 = *b1 = 0;
    if (!*table) return false;
    std::shared_lock<std::shared_mutex> lock(g_heapMutex);
    HeapRec* h = FindGpuLocked(*table);
    if (!h) return false;
    const UINT i = static_cast<UINT>((*table - h->gpuStart) / h->inc);
    if (i + kSkinSlot >= h->count) return false;
    *b0 = h->e[i + kWorldSlot].va;
    *b1 = h->e[i + kSkinSlot].va;
    return true;
}

// ---- Body learning (2026-10-12): which draws are Lara's body / outfit, whatever she wears. The body census showed her
// outfit is a separate model with its own WorldBuffer (b0) but, being her, the same World matrix as her face model. Every
// 3 s, for one frame, each colour draw's b0 address is noted; when the copy that fills it is recorded its World (b0
// +128) is read; afterwards the index buffers whose World equals the face's (same frame) are her body parts.
namespace {
struct BodyCand { uint64_t ib; uint32_t count, first; };
std::mutex g_bodyMutex;
std::multimap<uint64_t, BodyCand> g_bodyPending;   // b0 address -> draw (the learning frame's)
struct BodyRead { uint64_t ib; float w[16]; };
std::vector<BodyRead> g_bodyReads;
std::atomic<bool> g_bodyLearning{ false };
std::atomic<uint64_t> g_bodyIbs[8];                // learned body index buffers (0 = empty)
std::atomic<int> g_bodyIbCount{ 0 };
}  // namespace

bool FaceTrackBodyLearning() { return g_bodyLearning.load(std::memory_order_relaxed); }

void FaceTrackLearnDraw(const void* list, uint64_t ib, uint32_t count, uint32_t first) {
    uint64_t table, b0, b1;
    if (!FaceTrackDrawKeys(list, &table, &b0, &b1) || !b0) return;
    std::lock_guard<std::mutex> lock(g_bodyMutex);
    if (g_bodyPending.size() < 8000) g_bodyPending.emplace(b0, BodyCand{ ib, count, first });
}

namespace {
bool BodyLearnWanted() {
    std::lock_guard<std::mutex> lock(g_bodyMutex);
    return !g_bodyPending.empty();
}

// From hkCopyBuf: the World of every pending learning draw this copy fills.
void BodyLearnCopy(uint64_t lo, uint64_t hi, const uint8_t* cpu) {
    std::lock_guard<std::mutex> lock(g_bodyMutex);
    if (g_bodyPending.empty()) return;
    for (auto it = g_bodyPending.lower_bound(lo); it != g_bodyPending.end() && it->first + 192 <= hi;) {
        BodyRead r{ it->second.ib, {} };
        if (ReadGuarded(cpu + (it->first - lo) + 128, r.w, sizeof r.w) && g_bodyReads.size() < 8000) g_bodyReads.push_back(r);
        it = g_bodyPending.erase(it);
    }
}

}  // namespace

bool FaceTrackIsBodyIb(uint64_t ib) {
    const int n = g_bodyIbCount.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) if (g_bodyIbs[i].load(std::memory_order_relaxed) == ib) return true;
    return false;
}

// Present thread: one learning frame every 3 s; its results are judged 0.4 s later (the copies are recorded by then).
void FaceTrackBodyTick(uint64_t faceIb) {
    static double nextLearn = 0, judgeAt = 0;
    static bool learnedOnce = false;
    const double now = NowMs();
    if (g_bodyLearning.load()) {   // the learning frame is over
        g_bodyLearning.store(false);
        judgeAt = now + 400.0;
        return;
    }
    if (judgeAt > 0 && now >= judgeAt) {
        judgeAt = 0;
        std::vector<BodyRead> reads;
        {
            std::lock_guard<std::mutex> lock(g_bodyMutex);
            reads.swap(g_bodyReads);
            g_bodyPending.clear();
        }
        g_learnFaceOpen.store(false);
        if (!g_learnFaceOk.load()) return;   // no face reading in that frame: keep what we have
        std::vector<uint64_t> found;
        for (const BodyRead& r : reads) {
            if (r.ib == faceIb || std::find(found.begin(), found.end(), r.ib) != found.end()) continue;
            bool same = true;
            for (int k = 0; k < 16 && same; ++k) same = std::fabs(r.w[k] - g_learnFaceW[k]) <= (k >= 12 ? 0.5f : 1e-3f);
            if (same && found.size() < 8) found.push_back(r.ib);
        }
        // Publish when it changed (an outfit change, a new level); keep the old set if nothing matched this time.
        bool changed = static_cast<int>(found.size()) != g_bodyIbCount.load();
        for (size_t i = 0; i < found.size() && !changed; ++i) changed = !FaceTrackIsBodyIb(found[i]);
        if (!found.empty() && changed) {
            g_bodyIbCount.store(0);
            for (size_t i = 0; i < found.size(); ++i) g_bodyIbs[i].store(found[i]);
            g_bodyIbCount.store(static_cast<int>(found.size()));
            char b[120];
            snprintf(b, sizeof(b), "FACETRACK: Lara's body is %zu model%s drawn with her face's World matrix (learned from %zu draws)",
                     found.size(), found.size() == 1 ? "" : "s", reads.size());
            modlog::Line(b);
            learnedOnce = true;
        }
        (void)learnedOnce;
        return;
    }
    if (now >= nextLearn && judgeAt == 0) {
        nextLearn = now + 3000.0;
        g_learnFaceOk.store(false);
        g_learnFaceOpen.store(true);
        g_bodyLearning.store(true);
    }
}

bool FaceTrackIsMappedGpuMemory(uint64_t addr, uint64_t size) {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    for (const auto& kv : g_maps) {
        const uint64_t lo = reinterpret_cast<uint64_t>(kv.second.cpu);
        if (lo && addr + size > lo && addr < lo + kv.second.size) return true;
    }
    return false;
}

bool FaceRootPose(float pos[3], float axisYaw[2], double maxAgeMs) {
    const double t = g_rootMs.load(std::memory_order_acquire);
    if (t <= 0.0 || NowMs() - t > maxAgeMs) return false;
    for (int k = 0; k < 3; ++k) pos[k] = g_rootPos[k].load(std::memory_order_relaxed);
    axisYaw[0] = g_rootYaw[0].load(std::memory_order_relaxed);
    axisYaw[1] = g_rootYaw[1].load(std::memory_order_relaxed);
    return true;
}

std::string FaceTrackStats() {
    size_t heaps = 0, maps = 0;
    {
        std::shared_lock<std::shared_mutex> lock(g_heapMutex);
        heaps = g_heaps.size();
    }
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        maps = g_maps.size();
    }
    char b[320];
    float p[3];
    const bool fresh = FaceHeadPoint(p, 1000.0);
    snprintf(b, sizeof(b), "face draws %llu (%llu waited for their copy), head read %llu (last failure: %s); %zu CBV heaps, %zu mapped buffers, CBVs %llu, copied %llu, buffer copies %llu%s",
             static_cast<unsigned long long>(g_statFace.exchange(0)), static_cast<unsigned long long>(g_statPending.exchange(0)),
             static_cast<unsigned long long>(g_statOk.exchange(0)),
             kFailName[g_lastFail.exchange(0)], heaps, maps, static_cast<unsigned long long>(g_statCbv.exchange(0)),
             static_cast<unsigned long long>(g_statCopies.exchange(0)), static_cast<unsigned long long>(g_statCopyBuf.exchange(0)),
             fresh ? "" : "; no fresh head point");
    std::string out = b;
    if (fresh) {
        snprintf(b, sizeof(b), "; head at (%.1f, %.1f, %.1f)", p[0], p[1], p[2]);
        out += b;
    }
    return out;
}

}  // namespace vrmod
