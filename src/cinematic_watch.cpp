#include "build_config.h"
#include "cinematic_watch.h"

#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "game_addresses.h"
#include "logger.h"
#include "audio_cue.h"
#include "present_hook.h"
#include "camera_interceptor.h"
#include "head_tracking.h"
#include "settings.h"
#include "aer.h"

namespace vrmod {

namespace {

// The game's event-post function (FUN_1408e75b0(bus, &eventId, ...)), found by pattern (game_addresses.cpp). Each
// named event's id lives in a global written by the game's event registration; those are found at install from the
// event names (GameEventIdsResolve). knownRva below = where they were in the build this was written for (log / checks).

enum class Act { Begin, Pulse, End, LogOnly };

struct EventDef {
    const char* name;
    uintptr_t knownRva;
    Act act;
    ThirdPersonReason reason;
    float seconds;   // Begin: safety timeout; Pulse: hold time. Unused otherwise.
};

using R = ThirdPersonReason;
// Third person held after events with no end event of their own (interactions, ledges, grapple throw, rappel zone):
// 1.5 s (2026-10-09 user; was 5-10 s). Moments with an end event (cutscene, rope, wall crawl, ...) last until it.
constexpr float kAutoHold = 1.5f;
// Exploration (walking, swimming, ...) stays first-person; everything below switches to third person.
const EventDef kEvents[] = {
    { "evCinematicStarted",                        0x35C9100, Act::Begin,   R::Cinematic, 900.0f },
    { "evCinematicFinished",                       0x35C90FC, Act::End,     R::Cinematic, 0.0f },
    { "AttachToWallCrawl",                         0x35C90F0, Act::Begin,   R::WallCrawl, 180.0f },
    { "DetachFromWallCrawl",                       0x35C9118, Act::End,     R::WallCrawl, 0.0f },
    { "evGrappleAxeThrown",                        0x35C9054, Act::Begin,   R::Grapple,   40.0f },
    { "evGrappleAxeAttached",                      0x35C9060, Act::Begin,   R::Grapple,   40.0f },
    { "evGrappleProjectileAttachedToTerrain",      0x35C9048, Act::Begin,   R::Grapple,   40.0f },
    { "evGrappleSwingThrowRequested",              0x35C8FD8, Act::Pulse,   R::Grapple,   kAutoHold },
    { "evGrappleAxeDetached",                      0x35C9064, Act::End,     R::Grapple,   0.0f },
    { "evGrappleAxeRecovered",                     0x35C9068, Act::End,     R::Grapple,   0.0f },
    { "evGrappleAxeThrowFinished",                 0x35C905C, Act::LogOnly, R::Grapple,   0.0f },
    { "evRopeProjectileAttached",                  0x35C9070, Act::Begin,   R::Rope,      90.0f },
    { "evRopeProjectileAttachedOnAttachable",      0x35C9078, Act::Begin,   R::Rope,      90.0f },
    { "evRopeProjectileAttachedOnSwingable",       0x35C907C, Act::Begin,   R::Rope,      90.0f },
    { "evTrapezeProjectileAttachedToGrapplePoint", 0x35C9080, Act::Begin,   R::Rope,      60.0f },
    { "evRopeProjectile_NotAttached",              0x35C9074, Act::End,     R::Rope,      0.0f },
    { "evEnterRappelSwingInitiationZone",          0x35C8FD4, Act::Pulse,   R::Rappel,    kAutoHold },
    { "evPlayerClimbUpLedge",                      0x35C9010, Act::Pulse,   R::Ledge,     kAutoHold },
    { "evPlayerLedgeHangTypeChanged",              0x35C8FF8, Act::Pulse,   R::Ledge,     kAutoHold },
    { "uiplus_ledge_grab",                         0x35C90B8, Act::Begin,   R::Ledge,     30.0f },
    { "uiplus_ledge_grab_end",                     0x35C90BC, Act::End,     R::Ledge,     0.0f },
    { "OnInteractStart",                           0x35C9034, Act::Begin,   R::Interact,  kAutoHold },   // none of its end events fire for an examine
    { "OnInteractCancel",                          0x35C9038, Act::End,     R::Interact,  0.0f },
    { "uiplus_interact_end",                       0x35C90B0, Act::End,     R::Interact,  0.0f },
    { "interact_use_abort",                        0x35C8E30, Act::End,     R::Interact,  0.0f },
    { "display_finisher_icon_start",               0x35C8E24, Act::Begin,   R::Finisher,  20.0f },
    { "display_finisher_icon_stop",                0x35C8E28, Act::End,     R::Finisher,  0.0f },
    // Logged only, to learn what they carry / when they fire.
    { "uiplus_interact",                           0x35C90AC, Act::LogOnly, R::Interact,  0.0f },
    { "uiplus_button_prompt_ledge",                0x35C90A8, Act::LogOnly, R::Ledge,     0.0f },
    { "uiplus_button_rope_to_ledge",               0x35C90B4, Act::LogOnly, R::Ledge,     0.0f },
    { "evMenuStateChanged",                        0x35C9168, Act::LogOnly, R::Cinematic, 0.0f },
    { "lc2_MainMenu2Game",                         0x35C9160, Act::LogOnly, R::Cinematic, 0.0f },
};

// Unnamed events identified from J marks (2026-09-26): the ids are hashes, the same in every run.
// Takedown: 0x723A3742 fires once as it starts (23:03:47, 23:15:52 -- nowhere else); 0x982814C3 fires at its start and
// again at its end (~5 s later; only in takedowns). Third person from the first to the second.
constexpr uint32_t kTakedownStart = 0x723A3742;
constexpr uint32_t kTakedownEdge = 0x982814C3;
std::atomic<double> g_takedownSinceMs{ 0.0 };

// The opening intro (title cards, the plane, its break-up) runs without head tracking until it cuts to Lara in the
// gorge -- the set's edges show if you look around. A new game loads from the main menu (lc2_MainMenu2Game) straight
// into a cutscene; in both runs the cut to the gorge came 20-21 s into the third cutscene (42 s + 90 s + 21 s after the
// load). Ends there, or when gameplay starts.
std::atomic<double> g_fromMenuMs{ -1e9 };
std::atomic<uint64_t> g_playerFramesAtMenu{ 0 };   // PlayerCameraFrames() at the load from the menu
std::atomic<int> g_introCinematic{ 0 };          // 0 = no intro; n = inside the intro's n-th cutscene
std::atomic<double> g_introEndMs{ 0.0 };          // set when the third cutscene starts

using PostEvent_t = uintptr_t(__fastcall*)(void*, uint32_t*, uint64_t, uint64_t, uint64_t, uint64_t);
PostEvent_t g_origPost = nullptr;
constexpr int kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);
uintptr_t g_idAddr[kEventCount] = {};   // each event's id global in the running game (0 = not found: not watched)
std::atomic<bool> g_cinematic{false};
std::atomic<int> g_logged{0};

double NowMs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// Guarded copy (event payloads point into the game's stack / heap).
bool SafeCopy(void* dst, uintptr_t src, size_t n) {
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// New Game vs Continue (2026-10-10): evMenuStateChanged's payload starts { u32 phase, u32 from menu, u32 to menu }
// (phases 2, 1, 0 for one change). Leaving the title menu (6) for the loading screen (0x0E) goes through the New Game
// difficulty screen (0x32) for a new game and through 0x31 for Continue -- which also appears from the pause menu, so
// only 0x32 counts. A checkpoint that starts with a cutscene, continued from the title menu, had been taken for the
// intro and went flat (no player camera between the load and the cutscene, like a new game).
constexpr uint32_t kMenuLoading = 0x0E, kMenuNewGame = 0x32;
std::atomic<uint32_t> g_menuBeforeLoading{ 0xFFFFFFFFu };
std::atomic<bool> g_newGameLoad{ false };

void IntroOnEvent(const EventDef& e, uint64_t size, uint64_t payload) {
    const double now = NowMs();
    if (std::strcmp(e.name, "evMenuStateChanged") == 0) {
        uint32_t w[3];
        if (size >= 12 && payload && SafeCopy(w, static_cast<uintptr_t>(payload), sizeof w) && w[0] == 2 && w[2] == kMenuLoading) {
            g_menuBeforeLoading.store(w[1]);
        }
        return;
    }
    if (std::strcmp(e.name, "lc2_MainMenu2Game") == 0) {
        g_fromMenuMs.store(now);
        g_playerFramesAtMenu.store(PlayerCameraFrames());
        const uint32_t from = g_menuBeforeLoading.exchange(0xFFFFFFFFu);
        g_newGameLoad.store(from == kMenuNewGame);
        char b[128];
        snprintf(b, sizeof(b), "INTRO: left the title menu %s (menu 0x%X before loading)",
            from == kMenuNewGame ? "for a NEW GAME" : "for a saved game", from);
        modlog::Line(b);
        return;
    }
    if (std::strcmp(e.name, "evCinematicStarted") != 0) return;
    const int n = g_introCinematic.load();
    if (n == 0) {
        // A new game goes from the load straight into cutscenes; a loaded save shows gameplay first (2026-10-08: a short
        // duck-under cinematic 45 s after loading a save was taken for the intro and went flat). More than a second of
        // player camera since the load = not the intro. And only after New Game (see g_newGameLoad).
        const bool playedSinceLoad = PlayerCameraFrames() - g_playerFramesAtMenu.load() > 72;
        if (now - g_fromMenuMs.load() < 60000.0 && (playedSinceLoad || !g_newGameLoad.load())) g_fromMenuMs.store(-1e9);
        if (now - g_fromMenuMs.load() < 60000.0) {
            g_fromMenuMs.store(-1e9);
            g_introCinematic.store(1);
            modlog::Line("INTRO: a new game's opening -- head tracking off until the cut to Lara in the gorge.");
        }
    } else {
        g_introCinematic.store(n + 1);
        if (n + 1 == 3) g_introEndMs.store(now + 20000.0);
    }
}

// evMenuStateChanged's payload (args: 0, size 0x10, pointer), to learn whether New Game and Continue differ (2026-10-10:
// the intro detection can't tell them apart from the save files -- every save is read at the title screen): 32 bytes,
// and the text behind any of them that is a pointer to readable characters.
std::string MenuPayload(uint64_t size, uint64_t ptr) {
    std::ostringstream o;
    uint8_t buf[32];
    if (!ptr || !SafeCopy(buf, static_cast<uintptr_t>(ptr), sizeof buf)) return " payload unreadable";
    o << " payload[" << std::dec << size << "]=";
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) o << hx[buf[i] >> 4] << hx[buf[i] & 15] << ((i & 7) == 7 && i < 31 ? " " : "");
    for (int q = 0; q < 4; ++q) {
        uint64_t v;
        memcpy(&v, buf + q * 8, 8);
        if (v < 0x10000 || v > 0x7FFFFFFFFFFFull) continue;
        char s[49] = {};
        if (!SafeCopy(s, static_cast<uintptr_t>(v), 48)) continue;
        int len = 0;
        while (len < 48 && s[len] >= 32 && s[len] < 127) ++len;
        if (len >= 4 && (len == 48 || s[len] == 0)) o << " q" << q << "->\"" << std::string(s, len) << "\"";
    }
    return o.str();
}

void Handle(const EventDef& e, uint32_t* idPtr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    IntroOnEvent(e, b, c);
    // Logging pass: every watched event, with its raw arguments (the menu event's payload in particular).
    if (GetLogSwitches().events && g_logged.fetch_add(1) < 5000) {
        std::ostringstream o;
        o << "EVENT " << e.name << " [" << (e.act == Act::Begin ? "begin" : e.act == Act::End ? "end"
            : e.act == Act::Pulse ? "pulse" : "log-only") << "] args=0x" << std::hex << a << ",0x" << b << ",0x" << c << ",0x" << d;
        if (idPtr) {
            o << " id-block=";
            const uint32_t* w = idPtr;
            for (int i = 0; i < 4; ++i) o << "0x" << w[i] << (i < 3 ? "," : "");
        }
        if (std::strcmp(e.name, "evMenuStateChanged") == 0) o << MenuPayload(b, c);
        modlog::Line(o.str());
    }

    // A cutscene starts or ends: the game switches to another camera, so re-take "forward" from the head (AER).
    if (e.reason == R::Cinematic && (e.act == Act::Begin || e.act == Act::End) && GetRenderMode() == RenderMode::Aer) {
        AerRequestRecenter(e.act == Act::Begin ? "cutscene started" : "cutscene finished");
    }

    switch (e.act) {
    case Act::Begin:
        CameraReasonBegin(e.reason, e.seconds);
        break;
    case Act::Pulse:
        CameraReasonPulse(e.reason, e.seconds);
        break;
    case Act::End:
        CameraReasonEnd(e.reason);
        break;
    case Act::LogOnly:
        break;
    }

    if (e.reason == R::Cinematic && e.act != Act::LogOnly) {
        const bool starting = (e.act == Act::Begin);
        if (g_cinematic.exchange(starting) != starting) {
            HeadTrackingMarkDiscontinuity(starting ? "cinematic started" : "cinematic finished");
        }
    }
}

// Discovery (docs/FIRSTPERSON.md): some interactions (an examine without a menu, run 9) end with none of the known end
// events, so third person ran to the 25 s timeout. For 40 s after OnInteractStart every OTHER event posted is logged
// (first time each id is seen, with its time) to find the one that marks the end.
std::atomic<double> g_discoverUntilMs{ 0.0 };
std::atomic<uint32_t> g_seenIds[128];
std::atomic<int> g_seenCount{ 0 };

void Discover(uint32_t id, uint64_t a, uint64_t b) {
    if (NowMs() > g_discoverUntilMs.load(std::memory_order_relaxed)) return;
    const int n = (std::min)(g_seenCount.load(std::memory_order_relaxed), 128);
    for (int i = 0; i < n; ++i) if (g_seenIds[i].load(std::memory_order_relaxed) == id) return;
    const int slot = g_seenCount.fetch_add(1);
    if (slot >= 128) return;
    g_seenIds[slot].store(id);
    std::ostringstream o;
    o << "EVENTDISCOVER: new event id 0x" << std::hex << id << " args=0x" << a << ",0x" << b << std::dec << " (during an interaction)";
    modlog::Line(o.str());
}

// Event timeline (2026-09-26, finding the squeeze signal): with events logging on, every unnamed event is logged
// with its time -- the first time each id is seen, and again after 5 s of silence -- so the ids around a MARK (J)
// can be matched to what the player was doing. Lock-free id table; capped.
std::atomic<uint32_t> g_tlIds[512];
std::atomic<double> g_tlLastMs[512];
std::atomic<int> g_tlLines{ 0 };

void Timeline(uint32_t id, uint64_t a, uint64_t b) {
    if (!GetLogSwitches().events || g_tlLines.load(std::memory_order_relaxed) >= 4000) return;
    const double now = NowMs();
    size_t h = (id * 2654435761u) % 512;
    for (int probe = 0; probe < 512; ++probe, h = (h + 1) % 512) {
        uint32_t cur = g_tlIds[h].load(std::memory_order_relaxed);
        if (cur == 0) {
            uint32_t expected = 0;
            cur = g_tlIds[h].compare_exchange_strong(expected, id) ? id : expected;
            if (cur == id) g_tlLastMs[h].store(-1e9);
        }
        if (cur != id) continue;
        const double last = g_tlLastMs[h].exchange(now);
        if (now - last < 5000.0) return;
        g_tlLines.fetch_add(1);
        std::ostringstream o;
        o << "EVENTSEEN 0x" << std::hex << id << " args=0x" << a << ",0x" << b;
        modlog::Line(o.str());
        return;
    }
}

uintptr_t __fastcall hkPostEvent(void* bus, uint32_t* idPtr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    if (idPtr) {
        const uint32_t id = *idPtr;
        bool known = false;
        for (int i = 0; i < kEventCount; ++i) {
            const EventDef& e = kEvents[i];
            if (!g_idAddr[i]) continue;
            const uint32_t k = *reinterpret_cast<const uint32_t*>(g_idAddr[i]);
            if (k != 0 && k == id) {
                known = true;
                if (e.reason == R::Interact && e.act == Act::Begin) {
                    g_seenCount.store(0);
                    g_discoverUntilMs.store(NowMs() + 40000.0);
                }
                Handle(e, idPtr, a, b, c, d);
                break;
            }
        }
        if (!known) {
            if (id == kTakedownStart) {
                g_takedownSinceMs.store(NowMs());
                CameraReasonBegin(R::Finisher, 8.0);
                modlog::Line("EVENT takedown [begin]");
            } else if (id == kTakedownEdge && g_takedownSinceMs.load() > 0.0 && NowMs() - g_takedownSinceMs.load() > 1000.0) {
                g_takedownSinceMs.store(0.0);
                CameraReasonEnd(R::Finisher);
                modlog::Line("EVENT takedown [end]");
            }
            Discover(id, a, b);
            Timeline(id, a, b);
        }
    }
    return g_origPost(bus, idPtr, a, b, c, d);
}

} // namespace

// Present thread: the intro's end (timer, or gameplay for 3 s), and head tracking kept off while it runs (re-applied
// every frame, since the menu/loading logic restores it on its own schedule).
void IntroPoll() {
    static bool turnedOff = false;
    static double notCinematicSinceMs = 0.0;
    const int n = g_introCinematic.load();
    if (n == 0) return;
    const double now = NowMs();
    notCinematicSinceMs = CinematicActive() ? 0.0 : (notCinematicSinceMs == 0.0 ? now : notCinematicSinceMs);
    const bool timerDone = g_introEndMs.load() > 0.0 && now >= g_introEndMs.load();
    const bool gameplay = notCinematicSinceMs > 0.0 && now - notCinematicSinceMs > 3000.0;
    if (timerDone || gameplay) {
        g_introCinematic.store(0);
        g_introEndMs.store(0.0);
        notCinematicSinceMs = 0.0;
        modlog::Line(std::string("INTRO: over (") + (timerDone ? "the cut to the gorge" : "gameplay") + ") -- head tracking back on.");
        if (turnedOff) {
            turnedOff = false;
            SetHeadTrackingEnabled(true);
        }
        return;
    }
    if (IsHeadTrackingEnabled()) {
        SetHeadTrackingEnabled(false);
        turnedOff = true;
    }
}

// J (no modifiers): stamps MARK in the log, to line up events and camera modes with what the player did.
void EventMarkNow(const char* how) {
    static int n = 0;
    modlog::Line("MARK " + std::to_string(++n) + " (" + how + ")");
}

void EventMarkPollHotkey() {
    IntroPoll();
    static bool was = false;
    const bool down = (GetAsyncKeyState('J') & 0x8000) != 0;
    const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    if (down && !was && !mods) {
        EventMarkNow("J");
        PlayCue(Cue::On);
    }
    was = down;
}

bool CinematicActive() {
    return g_cinematic.load();
}

bool InstallCinematicWatch() {
    void* target = reinterpret_cast<void*>(GameAddress(GameAddr::PostEvent));
    if (!target) {
        modlog::Line("EventWatch: the game's event-post function wasn't found (different build?); event-driven camera switching off.");
        return false;
    }
    {
        const char* names[kEventCount];
        for (int i = 0; i < kEventCount; ++i) names[i] = kEvents[i].name;
        GameEventIdsResolve(names, kEventCount);
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::string missing;
        for (int i = 0; i < kEventCount; ++i) {
            g_idAddr[i] = GameEventIdAddress(kEvents[i].name);
            if (!g_idAddr[i]) missing += std::string(" ") + kEvents[i].name;
            else if (g_idAddr[i] - base != kEvents[i].knownRva) missing += std::string(" (") + kEvents[i].name + " moved)";
        }
        if (!missing.empty()) modlog::Line("EventWatch: events not found / moved:" + missing);
    }
    if (!EnsureMinHookInitialized()) {
        return false;
    }
    if (MH_CreateHook(target, &hkPostEvent, reinterpret_cast<void**>(&g_origPost)) != MH_OK || MH_EnableHook(target) != MH_OK) {
        modlog::Line("EventWatch: hook failed.");
        return false;
    }
    modlog::Line("EventWatch: installed on the game's event bus (" + std::to_string(sizeof(kEvents) / sizeof(kEvents[0]))
        + " events watched; first-person only for exploration).");
    return true;
}

} // namespace vrmod
