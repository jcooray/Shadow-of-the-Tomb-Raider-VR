#include "build_config.h"
#include "afw.h"
#include "dlss_probe.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "aer.h"
#include "camera_interceptor.h"
#include "game_addresses.h"
#include "logger.h"
#include "perf_stats.h"
#include "head_tracking.h"
#include "present_hook.h"
#include "audio_cue.h"
#include "settings.h"
#include "dlss_mv.h"
#include "gpu_frame_timer.h"
#include <d3d12.h>

namespace vrmod {

namespace {

// The game's DLSS wrappers (Ghidra FUN_14101b860 evaluate, FUN_14101b9c0 create, FUN_14101d140 ensure; docs/DLSS.md),
// found by pattern (game_addresses.cpp).

// evaluate(this, cmdList, color, output, depth, motionVectors, exposure, renderW, renderH, jitterX, jitterYbits
//          (sign-flipped by the wrapper), reset, node)
using Eval_t = void(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, uint32_t, uint32_t, float, uint32_t, char, uint32_t);
// create(this, cmdList, renderW, renderH, outW, outH, flag, node) -> bool in the low byte
using Create_t = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, char, uint32_t);
// ensure(this, cmdList, renderW, renderH, outW, outH, flag, node)
using Ensure_t = void(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, char, uint32_t);

// The game's create helper FUN_14101c3d0(cmdList, creationNodeMask, visibilityNodeMask, &handle, ngxParams,
// createParams {width, height, outWidth, outHeight, perfQuality, featureFlags, enableSubrects (byte)}) -> NGX result.
using CreateHelper_t = uint32_t(__fastcall*)(void*, uint32_t, uint32_t, void**, void*, const uint32_t*);
CreateHelper_t g_createHelper = nullptr;

// D1 (docs/DLSS.md): per-eye DLSS history under AER. The DLSS object's slot 1 (a node slot the game never uses on one
// GPU) gets a second feature for the same GPU, created with the game's own parameters right after its ensure; each
// evaluate is routed to slot 0 (left) or 1 (right) by the eye of the frame it renders -- the frame the next Present
// shows, i.e. AER's last paired frame + 1 (D0: the pipeline depth drifts, so the latest game frame can't be used).
std::atomic<bool> g_perEye{false};
std::atomic<void*> g_dlssObject{nullptr};    // the game's DLSS object (from the ensure hook)
std::atomic<uint32_t> g_outW{0}, g_outH{0};  // its DLSS output size
std::atomic<bool> g_slot1Failed{false};
std::atomic<bool> g_toggled{false};          // reset both histories at the next evaluate
std::atomic<uint64_t> g_routed[3] = {};      // left, right, unknown (slot 0)
std::atomic<uint64_t> g_mismatches{0}, g_d1Resets{0};

Eval_t g_origEval = nullptr;
Create_t g_origCreate = nullptr;
Ensure_t g_origEnsure = nullptr;

std::atomic<uint64_t> g_evals{0}, g_resets{0}, g_nodeMask{0}, g_ensures{0};
std::atomic<bool> g_evalHooked{false};   // DlssEvaluateCount means something
std::atomic<uint32_t> g_lastW{0}, g_lastH{0};
std::atomic<float> g_jxMin{1e9f}, g_jxMax{-1e9f};
std::atomic<int> g_detailed{0};
std::atomic<uint32_t> g_evalTid{0};
char g_eyeSeq[65] = {};   // eye of the latest game frame at 64 consecutive evaluates (L/R/?)
std::atomic<int> g_eyeSeqN{0};
std::atomic<uint64_t> g_evalGame{0};   // latest game frame at the latest evaluate

void StoreMin(std::atomic<float>& a, float v) { float p = a.load(); while (v < p && !a.compare_exchange_weak(p, v)) {} }
void StoreMax(std::atomic<float>& a, float v) { float p = a.load(); while (v > p && !a.compare_exchange_weak(p, v)) {} }

void __fastcall HkEval(void* self, void* cl, void* color, void* output, void* depth, void* mv, void* exposure,
                       uint32_t w, uint32_t h, float jx, uint32_t jyBits, char reset, uint32_t node) {
    g_evals.fetch_add(1, std::memory_order_relaxed);
    if (reset) g_resets.fetch_add(1, std::memory_order_relaxed);
    g_nodeMask.fetch_or(1ull << (node & 63), std::memory_order_relaxed);
    g_lastW.store(w, std::memory_order_relaxed);
    g_lastH.store(h, std::memory_order_relaxed);
    StoreMin(g_jxMin, jx);
    StoreMax(g_jxMax, jx);
    g_evalTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const uint64_t gf = CameraLatestGameFrame();
    g_evalGame.store(gf, std::memory_order_relaxed);
    const int eye = AerEyeOfFrame(gf);
    const int k = g_eyeSeqN.load(std::memory_order_relaxed);
    if (gf != 0 && eye >= 0 && k < 64) { g_eyeSeq[k] = eye == 0 ? 'L' : eye == 1 ? 'R' : '?'; g_eyeSeqN.store(k + 1); }
    if (g_detailed.fetch_add(1) < 40) {
        float jy;
        memcpy(&jy, &jyBits, 4);
        char b[320];
        snprintf(b, sizeof(b), "DLSS eval: this %p node %u render %ux%u jitter (%.4f, %.4f as passed) reset %d | color %p out %p depth %p mv %p "
                 "exposure %p | thread %lu, game frame %llu, AER eye %d, present %llu",
                 self, node, w, h, jx, jy, reset ? 1 : 0, color, output, depth, mv, exposure, GetCurrentThreadId(),
                 static_cast<unsigned long long>(gf), eye, static_cast<unsigned long long>(HeadTrackingGetPresentFrame()));
        modlog::Line(b);
    }
    GpuFrameTimerDlssMark(static_cast<ID3D12GraphicsCommandList*>(cl), 0);
    uint32_t useNode = node;
    char useReset = reset;
    int routedEye = -1;
    uint64_t routedFrame = 0;
    {
        // Render thread only.
        static bool pending[2] = { true, true };
        static bool unknown = true;
        static uint64_t prevPredicted = 0;
        char* o = static_cast<char*>(self);
        const bool slot1 = *reinterpret_cast<void**>(o + 0x28 + 0x10) != nullptr;
        if (g_toggled.exchange(false)) { pending[0] = pending[1] = true; useReset = 1; }
        // Per-eye only while AER runs: menus and head tracking off stop the game frames, and routing by a stale prediction
        // sent every evaluate to one eye with a reset each frame (17:26 run).
        const bool live = IsHeadTrackingEnabled() && !MenuScreenActive();
        if (g_perEye.load(std::memory_order_relaxed) && node == 0 && slot1 && live) {
            const uint64_t S = AerLastPairedFrame();
            // The Present after the previous evaluate should have been paired with the frame predicted then; if not, AER
            // resynced and that evaluate may have gone to the wrong eye: both histories start again.
            if (prevPredicted != 0 && S != 0 && S != prevPredicted) {
                g_mismatches.fetch_add(1, std::memory_order_relaxed);
                pending[0] = pending[1] = true;
            }
            if (MvTakeOffsetChange()) pending[0] = pending[1] = true;
            const uint64_t raw = S ? S + 1 : 0;
            const uint64_t frame = raw ? raw + static_cast<int64_t>(MvFrameOffset()) : 0;
            const int eye = frame ? AerEyeOfFrame(frame) : -1;
            prevPredicted = eye >= 0 ? raw : 0;
            if (eye >= 0) {
                MvFrameCheckOnEvaluate(static_cast<ID3D12GraphicsCommandList*>(cl), static_cast<ID3D12Resource*>(depth),
                                       static_cast<ID3D12Resource*>(mv), w, h, raw);
            }
            if (eye < 0) {
                unknown = true;
                g_routed[2].fetch_add(1, std::memory_order_relaxed);
            } else {
                if (unknown) { pending[0] = pending[1] = true; unknown = false; }
                useNode = static_cast<uint32_t>(eye);
                routedEye = eye;
                routedFrame = frame;
                if (reset) pending[1 - eye] = true;     // the game's reset (a cut) applies to both histories
                if (pending[eye]) { useReset = 1; pending[eye] = false; g_d1Resets.fetch_add(1, std::memory_order_relaxed); }
                g_routed[eye].fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            unknown = true;
            prevPredicted = 0;
        }
    }
    if (MvProbeArmed()) {
        const uint64_t S = AerLastPairedFrame();
        MvProbeOnEvaluate(static_cast<ID3D12GraphicsCommandList*>(cl), static_cast<ID3D12Resource*>(depth), static_cast<ID3D12Resource*>(mv),
                          w, h, S + 1, S ? AerEyeOfFrame(S + 1) : -1);
    }
    // AFW: this frame's depth, linearised, for the warp of the other eye at its Present.
    if (routedEye >= 0) AfwOnEvaluate(static_cast<ID3D12GraphicsCommandList*>(cl), static_cast<ID3D12Resource*>(depth), w, h, routedFrame);
    // D2: with per-eye history, DLSS gets motion vectors to the same eye two frames back.
    void* useMv = mv;
    if (routedEye >= 0) {
        if (ID3D12Resource* fixed = MvFixOnEvaluate(static_cast<ID3D12GraphicsCommandList*>(cl), static_cast<ID3D12Resource*>(depth),
                                                    static_cast<ID3D12Resource*>(mv), w, h, routedFrame)) {
            useMv = fixed;
        }
    }
    GpuFrameTimerDlssMark(static_cast<ID3D12GraphicsCommandList*>(cl), 1);
    g_origEval(self, cl, color, output, depth, useMv, exposure, w, h, jx, jyBits, useReset, useNode);
    GpuFrameTimerDlssMark(static_cast<ID3D12GraphicsCommandList*>(cl), 2);
}

uint64_t __fastcall HkCreate(void* self, void* cl, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, char flag, uint32_t node) {
    const uint64_t r = g_origCreate(self, cl, rw, rh, ow, oh, flag, node);
    char b[256];
    snprintf(b, sizeof(b), "DLSS create: this %p node %u render %ux%u -> output %ux%u flag %d -> %s; quality index %d, slot handle %p",
             self, node, rw, rh, ow, oh, flag, (r & 0xFF) ? "ok" : "FAILED", *reinterpret_cast<int*>(reinterpret_cast<char*>(self) + 0x1C),
             *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x28 + static_cast<size_t>(node) * 0x10));
    modlog::Line(b);
    return r;
}

void __fastcall HkEnsure(void* self, void* cl, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, char flag, uint32_t node) {
    g_ensures.fetch_add(1, std::memory_order_relaxed);
    if (node == 0) {
        g_dlssObject.store(self, std::memory_order_relaxed);
        g_outW.store(ow, std::memory_order_relaxed);
        g_outH.store(oh, std::memory_order_relaxed);
    }
    static uint32_t last[6] = {};
    const uint32_t cur[6] = { rw, rh, ow, oh, static_cast<uint32_t>(flag), node };
    if (memcmp(last, cur, sizeof cur) != 0) {
        memcpy(last, cur, sizeof cur);
        char b[200];
        snprintf(b, sizeof(b), "DLSS ensure: this %p node %u render %ux%u -> output %ux%u flag %d (thread %lu)", self, node, rw, rh, ow, oh, flag,
                 GetCurrentThreadId());
        modlog::Line(b);
    }
    g_origEnsure(self, cl, rw, rh, ow, oh, flag, node);
    char* o = static_cast<char*>(self);
    void** h0 = reinterpret_cast<void**>(o + 0x28);
    void** h1 = reinterpret_cast<void**>(o + 0x28 + 0x10);
    if (g_perEye.load() && g_createHelper && node == 0 && cl && *h0 && !*h1 && !g_slot1Failed.load() && *reinterpret_cast<char*>(o + 10) != 0) {
        EnterCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(o + 0x70));
        const uint32_t cp[7] = { rw, rh, ow, oh, static_cast<uint32_t>(*reinterpret_cast<int*>(o + 0x1C)), static_cast<uint32_t>((flag != 0) + 10), 0 };
        const uint32_t r = g_createHelper(cl, 1, 1, h1, *reinterpret_cast<void**>(o + 0x20), cp);
        LeaveCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(o + 0x70));
        char b[200];
        const bool failed = (r & 0xFFF00000u) == 0xBAD00000u || !*h1;
        snprintf(b, sizeof(b), "DLSS D1: second feature (right eye) in slot 1 for %ux%u -> %ux%u: %s (NGX 0x%08x, handle %p)", rw, rh, ow, oh,
                 failed ? "FAILED -- per-eye history off" : "created", r, *h1);
        modlog::Line(b);
        if (failed) { g_slot1Failed.store(true); *h1 = nullptr; }
        g_toggled.store(true);   // both histories start from this frame
    }
}

bool HookAt(GameAddr which, void* detour, void** orig, const char* name) {
    void* target = reinterpret_cast<void*>(GameAddress(which));
    if (!target) {
        modlog::Line(std::string("DLSS probe: ") + name + " not found (different game build?); not hooked.");
        return false;
    }
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        modlog::Line(std::string("DLSS probe: hooking ") + name + " failed.");
        return false;
    }
    return true;
}

} // namespace

bool DlssEyeHandles(uint64_t* left, uint64_t* right, uint32_t* outW, uint32_t* outH) {
    *left = *right = 0;
    *outW = g_outW.load(std::memory_order_relaxed);
    *outH = g_outH.load(std::memory_order_relaxed);
    char* o = static_cast<char*>(g_dlssObject.load(std::memory_order_relaxed));
    if (!o || !g_perEye.load(std::memory_order_relaxed) || !IsHeadTrackingEnabled() || MenuScreenActive()) return false;
    *left = reinterpret_cast<uint64_t>(*reinterpret_cast<void* volatile*>(o + 0x28));
    *right = reinterpret_cast<uint64_t>(*reinterpret_cast<void* volatile*>(o + 0x28 + 0x10));
    return *left && *right && *left != *right && *outW && *outH;
}

bool InstallDlssProbe() {
    if (!EnsureMinHookInitialized()) return false;
    const bool a = HookAt(GameAddr::DlssEvaluate, reinterpret_cast<void*>(&HkEval), reinterpret_cast<void**>(&g_origEval), "evaluate");
    g_evalHooked.store(a);
    const bool b = HookAt(GameAddr::DlssCreate, reinterpret_cast<void*>(&HkCreate), reinterpret_cast<void**>(&g_origCreate), "create");
    const bool c = HookAt(GameAddr::DlssEnsure, reinterpret_cast<void*>(&HkEnsure), reinterpret_cast<void**>(&g_origEnsure), "ensure");
    g_createHelper = reinterpret_cast<CreateHelper_t>(GameAddress(GameAddr::DlssCreateHelper));
    g_perEye.store(GetDlssPerEye() && g_createHelper && a && c);
    modlog::Line(std::string("DLSS D1: per-eye history ") + (g_perEye.load() ? "ON" : "off") + " ([render] dlss_per_eye; Ctrl+Alt+D toggles)"
        + (g_createHelper ? "" : " -- create helper not found"));
    modlog::Line(std::string("DLSS probe: evaluate ") + (a ? "hooked" : "-") + ", create " + (b ? "hooked" : "-") + ", ensure " + (c ? "hooked" : "-")
        + " (the game's own DLSS wrappers; logging only).");
    return a || b || c;
}

int64_t DlssEvaluateCount() {
    return g_evalHooked.load() ? static_cast<int64_t>(g_evals.load()) : -1;
}

int DlssPerEyeState() {
    if (!g_createHelper || g_slot1Failed.load()) return -1;
    return g_perEye.load() ? 1 : 0;
}

bool DlssPerEyeSet(bool on) {
    if (!g_createHelper || g_slot1Failed.load()) return false;
    if (g_perEye.exchange(on) != on) {
        g_toggled.store(true);
        modlog::Line(std::string("DLSS D1: per-eye history ") + (on ? "ON" : "off") + " (overlay)");
    }
    return true;
}

void DlssProbeOnPresent() {
    MvProbeOnPresent();
    // Ctrl+Alt+D: per-eye history on/off (A/B); both histories reset.
    {
        static bool dWas = false;
        const bool d = (VRMOD_DEV_KEY('D') & 0x8000) != 0;
        const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
        if (d && !dWas && ca) {
            if (!g_createHelper || g_slot1Failed.load()) {
                modlog::Line("DLSS D1: per-eye history unavailable (see the earlier D1 line).");
                PlayCue(Cue::Failure);
            } else {
                const bool on = !g_perEye.load();
                g_perEye.store(on);
                g_toggled.store(true);
                modlog::Line(std::string("DLSS D1: per-eye history ") + (on ? "ON" : "off") + " (Ctrl+Alt+D)");
                PlayCue(on ? Cue::On : Cue::Off);
            }
        }
        dWas = d;
        static double lastD1Ms = 0.0;
        if (NowMs() - lastD1Ms >= 10000.0) {
            lastD1Ms = NowMs();
            const uint64_t l = g_routed[0].exchange(0), r = g_routed[1].exchange(0), u = g_routed[2].exchange(0);
            if (l + r + u > 0) {
                char b[240];
                snprintf(b, sizeof(b), "DLSS D1 (10 s, %s): evaluates to left %llu, right %llu, eye unknown (slot 0) %llu | AER resyncs seen %llu, history resets %llu",
                         g_perEye.load() ? "per-eye" : "off", static_cast<unsigned long long>(l), static_cast<unsigned long long>(r),
                         static_cast<unsigned long long>(u), static_cast<unsigned long long>(g_mismatches.exchange(0)),
                         static_cast<unsigned long long>(g_d1Resets.exchange(0)));
                modlog::Line(b);
            }
        }
    }
    static double lastMs = 0.0;
    static uint64_t lastEvals = 0, lastPresents = 0;
    static bool seqLogged = false;
    if (!seqLogged && g_eyeSeqN.load() >= 64) {
        seqLogged = true;
        modlog::Line(std::string("DLSS eval: AER eye of the latest game frame at 64 consecutive evaluates: ") + g_eyeSeq);
    }
    if (NowMs() - lastMs < 5000.0) return;
    lastMs = NowMs();
    const uint64_t e = g_evals.load(), p = HeadTrackingGetPresentFrame();
    if (e != lastEvals) {
        char b[256];
        snprintf(b, sizeof(b), "DLSS (5 s): %llu evaluates (%llu ensures) over %llu Presents, %llu resets, nodes mask 0x%llx, render %ux%u, jitter x %.3f..%.3f, thread %lu",
                 static_cast<unsigned long long>(e - lastEvals), static_cast<unsigned long long>(g_ensures.exchange(0)), static_cast<unsigned long long>(p - lastPresents),
                 static_cast<unsigned long long>(g_resets.exchange(0)), static_cast<unsigned long long>(g_nodeMask.load()), g_lastW.load(),
                 g_lastH.load(), g_jxMin.exchange(1e9f), g_jxMax.exchange(-1e9f), g_evalTid.load());
        modlog::Line(b);
    }
    lastEvals = e;
    lastPresents = p;
}

} // namespace vrmod
