#include "gear_hide.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "aer.h"
#include "audio_cue.h"
#include "camera_interceptor.h"
#include "logger.h"
#include "perf_stats.h"
#include "settings.h"

namespace vrmod {

namespace {

// Game addresses (image base 0x140000000), read from Ghidra (docs/FIRSTPERSON.md, "Gear hiding").
constexpr uintptr_t kInstanceTableRva = 0x301D0C0;   // 20000 entries of 0x120: +0x110 handle check, +0x88 parent
                                                     // handle, +0xD0 component count, +0xD8 component array
constexpr uintptr_t kDirectorRva = 0x16BB1F0;        // camera director: slot map, slot 0's object +0x20 = Lara's handle
constexpr uint32_t kDrawableIndex = 8;               // component 8 = cdc::DrawableComponent
// FUN_140447ae0 (what the animation graph's AGInstanceHideUnhide calls) hides an instance's drawable like this:
//   hide: if (!(d+0xD8 & 1)) MarkDirty(d); d+0xD8 |= 1; ClearVisible(d);
//   show: if (d+0xD8 & 1)    MarkDirty(d); d+0xD8 &= ~1; SetVisible(d);
// MarkDirty queues the drawable's render items for the renderer (under the game's own lock); Clear/SetVisible clear or
// set bit 0x100 on each render item. It also switches collision and lights, which we leave alone.
constexpr uintptr_t kMarkDirtyRva = 0x43AC70;
constexpr uintptr_t kClearVisibleRva = 0x436AC0;
constexpr uintptr_t kSetVisibleRva = 0x44A1C0;
constexpr unsigned char kMarkDirtyPrologue[] = { 0x40, 0x55, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0x79, 0x48, 0x00, 0x48,
                                                 0x8B, 0xE9, 0x0F, 0x84, 0x11, 0x01, 0x00, 0x00 };
constexpr unsigned char kVisiblePrologue[] = { 0x48, 0x83, 0x79, 0x70, 0x00, 0x4C, 0x8B, 0xC9, 0x74, 0x70, 0x48, 0x8B,
                                               0x51, 0x78, 0x33, 0xC0, 0x48, 0x85, 0xD2, 0x74, 0x65 };

// Objects that handle hiding themselves: FUN_14085a250 (the hide node's per-instance step) checks the instance's +0x68
// handle -> that entry's component 9 -> its +0x48; if set, it doesn't touch the drawable but sends the instance event
// 0x3EC04619 with "hide" as the argument, through FUN_1408e6d70(bus DAT_1436aaf30, instance, event, hide) -- the
// game's standard send-event-to-instance (bus listeners, then the instance's components, under the bus's lock).
constexpr uintptr_t kPostEventRva = 0x8E6D70;
constexpr uintptr_t kEventBusRva = 0x36AAF30;        // pointer to the event bus
constexpr uint32_t kHideEvent = 0x3EC04619;
constexpr unsigned char kPostEventPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57,
                                                 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x70 };

using DrawableFn = void(__fastcall*)(uintptr_t drawable);
DrawableFn g_markDirty = nullptr, g_clearVisible = nullptr, g_setVisible = nullptr;
using PostEventFn = void(__fastcall*)(uintptr_t bus, uintptr_t instance, uint32_t event, uint64_t arg);
PostEventFn g_postEvent = nullptr;

uintptr_t Exe() {
    static const uintptr_t e = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return e;
}

template <typename T>
bool Rd(uintptr_t a, T* out) {
    if (a < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const volatile T*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ResolveFunctions() {
    static int state = 0;   // 0 not tried, 1 ok, 2 mismatch
    if (state == 0) {
        const auto* dirty = reinterpret_cast<const unsigned char*>(Exe() + kMarkDirtyRva);
        const auto* clear = reinterpret_cast<const unsigned char*>(Exe() + kClearVisibleRva);
        const auto* set = reinterpret_cast<const unsigned char*>(Exe() + kSetVisibleRva);
        const bool ok = memcmp(dirty, kMarkDirtyPrologue, sizeof kMarkDirtyPrologue) == 0
            && memcmp(clear, kVisiblePrologue, sizeof kVisiblePrologue) == 0 && memcmp(set, kVisiblePrologue, sizeof kVisiblePrologue) == 0
            && memcmp(reinterpret_cast<const unsigned char*>(Exe() + kPostEventRva), kPostEventPrologue, sizeof kPostEventPrologue) == 0;
        if (ok) {
            g_postEvent = reinterpret_cast<PostEventFn>(Exe() + kPostEventRva);
            g_markDirty = reinterpret_cast<DrawableFn>(Exe() + kMarkDirtyRva);
            g_clearVisible = reinterpret_cast<DrawableFn>(Exe() + kClearVisibleRva);
            g_setVisible = reinterpret_cast<DrawableFn>(Exe() + kSetVisibleRva);
        }
        state = ok ? 1 : 2;
        modlog::Line(ok ? "GEARHIDE: game hide/show functions found." : "GEARHIDE: the game's hide/show functions don't match this build; gear hiding off.");
    }
    return state == 1;
}

// Instance table entry for a handle (checked), or 0.
uintptr_t Entry(uint64_t handle) {
    if ((handle >> 32) == 0 || static_cast<uint32_t>(handle) >= 20000) return 0;
    const uintptr_t e = Exe() + kInstanceTableRva + static_cast<uintptr_t>(static_cast<uint32_t>(handle)) * 0x120;
    uint64_t check = 0;
    return Rd(e + 0x110, &check) && check == handle ? e : 0;
}

uintptr_t Component(uintptr_t entry, uint32_t index) {
    uint32_t count = 0;
    uintptr_t comps = 0, c = 0;
    if (!Rd(entry + 0xD0, &count) || index >= count || count > 512 || !Rd(entry + 0xD8, &comps) || !comps) return 0;
    return Rd(comps + static_cast<uintptr_t>(index) * 8, &c) ? c : 0;
}

// Lara: the camera director's slot 0 object (FUN_140170bd0 -> FUN_140188dc0) -- an instance with PlayerComponent,
// InventoryComponent and CameraComponent (2026-09-30 dump).
uint64_t LaraHandle() {
    uintptr_t dir = 0, buckets = 0, count = 0, node = 0;
    if (!Rd(Exe() + kDirectorRva, &dir) || !dir || !Rd(dir + 0x68, &buckets) || !Rd(dir + 0x80, &count) || !buckets || !count) return 0;
    if (!Rd(buckets, &node)) return 0;
    for (int guard = 0; node && guard < 64; ++guard) {
        uint32_t key = ~0u;
        if (!Rd(node + 0x10, &key)) return 0;
        if (key == 0) {
            uintptr_t obj = 0;
            uint64_t h = 0;
            if (!Rd(node + 0x520, &obj) || !obj || !Rd(obj + 0x20, &h)) return 0;
            return h;
        }
        if (!Rd(node, &node)) return 0;
    }
    return 0;
}

// RTTI class name of an object (vtable[-1] -> complete object locator), for the log.
std::string ClassOf(uintptr_t obj) {
    uintptr_t vt = 0, col = 0;
    uint32_t sig = 0, td = 0;
    if (!Rd(obj, &vt) || !Rd(vt - 8, &col) || !Rd(col, &sig) || sig != 1 || !Rd(col + 0x0C, &td)) return "?";
    char buf[96] = {};
    for (int i = 0; i < 95; ++i) {
        char c = 0;
        if (!Rd(Exe() + td + 0x10 + i, &c) || !c) break;
        buf[i] = c;
    }
    std::string s = buf;
    if (s.rfind(".?AV", 0) == 0) s = s.substr(4);
    const size_t at = s.find("@@");
    if (at != std::string::npos) s = s.substr(0, at);
    return s;
}

// A readable ASCII name at p (4+ name-like characters, NUL-terminated), or "".
std::string NameAt(uintptr_t p) {
    char buf[64] = {};
    int n = 0;
    for (; n < 63; ++n) {
        char c = 0;
        if (!Rd(p + n, &c)) return "";
        if (!c) break;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/' || c == '\\';
        if (!ok) return "";
        buf[n] = c;
    }
    return n >= 4 && n < 63 ? std::string(buf, n) : "";
}

// Names found near an instance's object and mesh components (pointers to strings, one level deep), for the log.
std::string NameHints(uintptr_t entry) {
    std::string out;
    int found = 0;
    for (uint32_t ci : { 9u, 11u }) {
        const uintptr_t c = Component(entry, ci);
        if (!c) continue;
        for (uintptr_t off = 0; off < 0x100 && found < 4; off += 8) {
            uintptr_t p = 0;
            if (!Rd(c + off, &p) || p < 0x10000) continue;
            std::string s = NameAt(p);
            if (s.empty()) {   // one level further: a struct whose first fields point to a name
                for (uintptr_t o2 = 0; o2 < 0x20 && s.empty(); o2 += 8) {
                    uintptr_t q = 0;
                    if (Rd(p + o2, &q) && q >= 0x10000) s = NameAt(q);
                }
            }
            if (!s.empty() && out.find(s) == std::string::npos) {
                out += (out.empty() ? "" : ", ") + s;
                ++found;
            }
        }
    }
    return out.empty() ? "(no name found)" : out;
}

// Whether the game hides this instance with the hide event (FUN_14085a250's test) rather than through its drawable.
bool UsesHideEvent(uintptr_t entry) {
    uint64_t h = 0;
    if (!Rd(entry + 0x68, &h)) return false;
    const uintptr_t e2 = Entry(h);
    const uintptr_t c9 = e2 ? Component(e2, 9) : 0;
    uintptr_t v = 0;
    return c9 && Rd(c9 + 0x48, &v) && v != 0;
}

// ---- Lara's children (found by the Present thread, used by the game thread) ----
struct Child { uint64_t handle; uintptr_t entry, drawable; uint32_t components; bool eventPath; std::string classes, names; };
std::mutex g_mutex;
std::vector<Child> g_children;       // drawable children of Lara, in table order
uint64_t g_lara = 0;
std::atomic<int> g_mode{-1};         // -1 not loaded; 0 off; 1 all; 2.. only child (mode-2)
std::atomic<bool> g_logList{false};
std::atomic<int> g_hiddenNow{0};

int Mode() {
    int m = g_mode.load(std::memory_order_relaxed);
    if (m < 0) {
        m = GetHideGearSetting() ? 1 : 0;
        g_mode.store(m);
    }
    return m;
}

// Present thread, every 2 s: every instance whose parent (+0x88) is Lara and that has a drawable.
void ScanChildren() {
    const uint64_t lara = LaraHandle();
    std::vector<Child> kids;
    if (lara && Entry(lara)) {
        for (uint32_t i = 0; i < 20000 && kids.size() < 64; ++i) {
            const uintptr_t e = Exe() + kInstanceTableRva + static_cast<uintptr_t>(i) * 0x120;
            uint64_t own = 0, parent = 0;
            if (!Rd(e + 0x110, &own) || (own >> 32) == 0 || static_cast<uint32_t>(own) != i) continue;
            if (!Rd(e + 0x88, &parent) || parent != lara) continue;
            const uintptr_t d = Component(e, kDrawableIndex);
            if (!d || ClassOf(d) != "DrawableComponent@cdc") continue;
            Child c{ own, e, d, 0, UsesHideEvent(e), "", "" };
            Rd(e + 0xD0, &c.components);
            kids.push_back(c);
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    bool changed = lara != g_lara || kids.size() != g_children.size();
    for (size_t i = 0; !changed && i < kids.size(); ++i) changed = kids[i].handle != g_children[i].handle;
    if (!changed) return;
    // Names and classes only for the log, when the set changes.
    for (Child& c : kids) {
        std::ostringstream o;
        for (uint32_t ci = 0; ci < c.components && ci < 24; ++ci) {
            const uintptr_t p = Component(c.entry, ci);
            if (!p) continue;
            std::string n = ClassOf(p);
            const size_t at = n.find("@cdc");
            if (at != std::string::npos) n = n.substr(0, at);
            if (n == "TransformComponent" || n == "HierarchyComponent" || n == "DrawableComponent" || n == "ObjectComponent") continue;
            o << (o.tellp() > 0 ? " " : "") << n;
        }
        c.classes = o.str();
        c.names = NameHints(c.entry);
    }
    g_lara = lara;
    g_children.swap(kids);
    g_logList.store(true);
}

// Watcher (background thread, every 250 ms): the hidden flag (drawable +0xD8 bit 0) of every instance that has a
// DrawableComponent, anywhere in the table; changes are logged with the instance's parent chain up to Lara. Swapping
// weapons hides the bow somewhere -- this finds where.
std::string ParentChain(uint64_t handle, uint64_t lara) {
    std::ostringstream o;
    uint64_t h = handle;
    for (int depth = 0; depth < 8; ++depth) {
        const uintptr_t e = Entry(h);
        if (!e) { o << " -> (gone)"; break; }
        uint64_t parent = 0;
        if (!Rd(e + 0x88, &parent) || !parent) { o << " -> (root)"; break; }
        o << " -> 0x" << std::hex << parent << std::dec;
        if (parent == lara) { o << " (LARA, depth " << depth + 1 << ")"; break; }
        h = parent;
    }
    return o.str();
}

std::string ComponentClasses(uintptr_t entry) {
    uint32_t count = 0;
    Rd(entry + 0xD0, &count);
    std::ostringstream o;
    for (uint32_t ci = 0; ci < count && ci < 24; ++ci) {
        const uintptr_t p = Component(entry, ci);
        if (!p) continue;
        std::string n = ClassOf(p);
        const size_t at = n.find("@cdc");
        if (at != std::string::npos) n = n.substr(0, at);
        if (n == "TransformComponent" || n == "HierarchyComponent" || n == "DrawableComponent" || n == "ObjectComponent") continue;
        o << (o.tellp() > 0 ? " " : "") << n;
    }
    return o.str();
}

void WatchHiddenFlags() {
    static std::unordered_map<uint64_t, uintptr_t> drawableOf;   // handle -> drawable (0 = has none)
    static std::unordered_map<uint64_t, uint8_t> flagOf;
    static int logged = 0;
    static bool primed = false;
    uint64_t lara = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        lara = g_lara;
    }
    std::string out;
    int changes = 0;
    for (uint32_t i = 0; i < 20000; ++i) {
        const uintptr_t e = Exe() + kInstanceTableRva + static_cast<uintptr_t>(i) * 0x120;
        uint64_t own = 0;
        if (!Rd(e + 0x110, &own) || (own >> 32) == 0 || static_cast<uint32_t>(own) != i) continue;
        auto it = drawableOf.find(own);
        if (it == drawableOf.end()) {
            const uintptr_t d = Component(e, kDrawableIndex);
            it = drawableOf.emplace(own, d && ClassOf(d) == "DrawableComponent@cdc" ? d : 0).first;
        }
        if (!it->second) continue;
        uint8_t f = 0;
        if (!Rd(it->second + 0xD8, &f)) continue;
        f &= 1;
        auto fl = flagOf.find(own);
        if (fl == flagOf.end()) { flagOf.emplace(own, f); continue; }
        if (fl->second == f) continue;
        fl->second = f;
        if (!primed || logged >= 300) continue;
        ++changes;
        ++logged;
        uint32_t count = 0;
        Rd(e + 0xD0, &count);
        std::ostringstream o;
        o << "GEARWATCH: 0x" << std::hex << own << std::dec << " now " << (f ? "HIDDEN" : "shown") << ", " << count << " components ["
          << ComponentClasses(e) << "] parents" << ParentChain(own, lara);
        out += o.str() + "\n";
    }
    primed = true;
    if (!out.empty()) modlog::Line(out.substr(0, out.size() - 1));
}

// Game thread: hide / show one drawable the way FUN_140447ae0 does (visual only).
void Hide(uintptr_t d) {
    uint8_t flags = 0;
    if (!Rd(d + 0xD8, &flags)) return;
    if (!(flags & 1)) g_markDirty(d);
    *reinterpret_cast<volatile uint8_t*>(d + 0xD8) = static_cast<uint8_t>(flags | 1);
    g_clearVisible(d);
}

void Show(uintptr_t d) {
    uint8_t flags = 0;
    if (!Rd(d + 0xD8, &flags)) return;
    if (flags & 1) g_markDirty(d);
    *reinterpret_cast<volatile uint8_t*>(d + 0xD8) = static_cast<uint8_t>(flags & ~1u);
    g_setVisible(d);
}

std::vector<uint64_t> g_hidden;   // game thread: handles we hid (their drawables are shown again when we stop)

// What a drawable looks like to the renderer: its hidden flag, how many render items it has in the dirty map (+0x38
// buckets, +0x48 count, +0x50 bucket count) and in the visibility map (+0x60 / +0x70 / +0x78), and how many of the
// latter have the visible bit (0x100 at item+0x90).
std::string DrawableState(uintptr_t d) {
    uint8_t flag = 0;
    uint64_t n1 = 0, n2 = 0, b2 = 0;
    uintptr_t buckets = 0;
    Rd(d + 0xD8, &flag);
    Rd(d + 0x48, &n1);
    Rd(d + 0x70, &n2);
    Rd(d + 0x78, &b2);
    Rd(d + 0x60, &buckets);
    int items = 0, visible = 0;
    for (uint64_t b = 0; buckets && b < b2 && b < 4096 && items < 256; ++b) {
        uintptr_t node = 0;
        if (!Rd(buckets + b * 8, &node)) break;
        for (int guard = 0; node && guard < 64; ++guard) {
            uintptr_t item = 0;
            uint32_t f = 0;
            if (!Rd(node + 0x10, &item) || !Rd(item + 0x90, &f)) break;
            ++items;
            visible += (f & 0x100) ? 1 : 0;
            if (!Rd(node, &node)) break;
        }
    }
    char b[160];
    snprintf(b, sizeof(b), "hidden flag %d, dirty-map items %llu, visibility-map items %llu (walked %d, %d visible)", flag & 1,
             static_cast<unsigned long long>(n1), static_cast<unsigned long long>(n2), items, visible);
    return b;
}
std::atomic<bool> g_probeRequest{false};
std::atomic<int> g_probeCountdown{0};

} // namespace

void GearHideGameThread(uint64_t gameFrame, bool firstPersonActive) {
    static uint64_t lastFrame = ~0ull;
    if (gameFrame == lastFrame) return;
    lastFrame = gameFrame;
    if (!ResolveFunctions()) return;
    std::vector<Child> kids;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        kids = g_children;
    }
    const int mode = Mode();
    const bool active = firstPersonActive && !CameraAimingNow() && mode != 0;
    const int only = mode >= 2 ? mode - 2 : -1;
    // Hide what should be hidden (again, if the game showed it); show what we hid and no longer should.
    for (size_t i = 0; i < kids.size(); ++i) {
        const Child& c = kids[i];
        if (Entry(c.handle) != c.entry || Component(c.entry, kDrawableIndex) != c.drawable) continue;   // gone or changed
        const bool want = active && (only < 0 || static_cast<int>(i) == only);
        bool ours = false;
        for (uint64_t h : g_hidden) ours = ours || h == c.handle;
        uint8_t skip = 0;
        if (Rd(c.entry + 0xC4, &skip) && (skip & 1)) continue;   // FUN_14085a250 leaves these alone
        if (c.eventPath) {
            uintptr_t bus = 0;
            if (!Rd(Exe() + kEventBusRva, &bus) || !bus) continue;
            if (want && !ours) {
                g_postEvent(bus, c.entry, kHideEvent, 1);
                g_hidden.push_back(c.handle);
            } else if (!want && ours) {
                g_postEvent(bus, c.entry, kHideEvent, 0);
                for (size_t k = 0; k < g_hidden.size(); ++k) {
                    if (g_hidden[k] == c.handle) { g_hidden.erase(g_hidden.begin() + static_cast<long>(k)); break; }
                }
            }
            continue;
        }
        uint8_t flag = 0;
        if (!Rd(c.drawable + 0xD8, &flag)) continue;
        if (want) {
            if (!ours && (flag & 1)) continue;   // hidden by the game itself: not ours to touch
            if (!(flag & 1)) Hide(c.drawable);
            if (!ours) g_hidden.push_back(c.handle);
        } else if (ours) {
            if (flag & 1) Show(c.drawable);
            for (size_t k = 0; k < g_hidden.size(); ++k) {
                if (g_hidden[k] == c.handle) { g_hidden.erase(g_hidden.begin() + static_cast<long>(k)); break; }
            }
        }
    }
    // Handles no longer among Lara's children (unloaded or detached): show them if they still exist, then forget them.
    for (size_t k = 0; k < g_hidden.size();) {
        bool present = false;
        for (const Child& c : kids) present = present || c.handle == g_hidden[k];
        if (present) { ++k; continue; }
        const uintptr_t e = Entry(g_hidden[k]);
        const uintptr_t d = e ? Component(e, kDrawableIndex) : 0;
        uint8_t flag = 0;
        uintptr_t bus = 0;
        if (e && UsesHideEvent(e) && Rd(Exe() + kEventBusRva, &bus) && bus) g_postEvent(bus, e, kHideEvent, 0);
        else if (d && ClassOf(d) == "DrawableComponent@cdc" && Rd(d + 0xD8, &flag) && (flag & 1)) Show(d);
        g_hidden.erase(g_hidden.begin() + static_cast<long>(k));
    }
    g_hiddenNow.store(static_cast<int>(g_hidden.size()));
    if (g_probeRequest.exchange(false)) {
        std::string out;
        uint64_t lara = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            lara = g_lara;
        }
        const uintptr_t le = lara ? Entry(lara) : 0;
        const uintptr_t ld = le ? Component(le, kDrawableIndex) : 0;
        if (ld) out += "GEARHIDE probe Lara herself: " + DrawableState(ld) + "\n";
        for (size_t i = 0; i < kids.size(); ++i) {
            bool ours = false;
            for (uint64_t h : g_hidden) ours = ours || h == kids[i].handle;
            out += "GEARHIDE probe #" + std::to_string(i + 1) + (ours ? " (ours)" : "") + (kids[i].eventPath ? " [hide event]" : " [drawable]")
                + ": " + DrawableState(kids[i].drawable) + "\n";
        }
        modlog::Line(out.empty() ? "GEARHIDE probe: no children" : out.substr(0, out.size() - 1));
    }
}

void GearHideOnPresent() {
    // The table scan (20000 entries) runs on its own thread every 2 s, off the Present and game threads.
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            // WatchHiddenFlags() (GEARWATCH, table-wide hidden-flag changes) found nothing of Lara's on weapon swaps
            // (2026-09-30); kept for reference, not run.
            for (;;) {
                ScanChildren();
                Sleep(2000);
            }
        }).detach();
    });
    static bool gWas = false;
    const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool gDown = (GetAsyncKeyState('G') & 0x8000) != 0;
    if (gDown && !gWas && ca) {
        // Cycle: all -> only #1 -> ... -> only #n -> off -> all.
        int n = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            n = static_cast<int>(g_children.size());
        }
        int m = Mode();
        m = m == 0 ? 1 : (m == 1 ? (n > 0 ? 2 : 0) : (m - 1 < n ? m + 1 : 0));
        g_mode.store(m);
        g_logList.store(true);
        PlayCue(m == 0 ? Cue::Off : Cue::On);
    }
    gWas = gDown;
    {
        static double probeAt[2] = { 0, 0 };
        if (g_probeCountdown.exchange(0)) { probeAt[0] = NowMs() + 1000.0; probeAt[1] = NowMs() + 3000.0; }
        for (double& t : probeAt) {
            if (t > 0 && NowMs() >= t) { t = 0; g_probeRequest.store(true); }
        }
        static double lastHiddenProbe = 0.0;
        if (g_hiddenNow.load() > 0 && NowMs() - lastHiddenProbe > 5000.0) {
            lastHiddenProbe = NowMs();
            g_probeRequest.store(true);
        }
    }
    if (!g_logList.exchange(false)) return;
    g_probeCountdown.store(1);
    std::vector<Child> kids;
    uint64_t lara = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        kids = g_children;
        lara = g_lara;
    }
    const int mode = Mode();
    const std::string modeName = mode == 0 ? "off" : mode == 1 ? "ALL" : ("only #" + std::to_string(mode - 1));
    char head[200];
    snprintf(head, sizeof(head), "GEARHIDE: mode %s | Lara 0x%llx, %zu drawable children, %d hidden now (shown while aiming or in third person)",
             modeName.c_str(), static_cast<unsigned long long>(lara), kids.size(), g_hiddenNow.load());
    modlog::Line(head);
    for (size_t i = 0; i < kids.size(); ++i) {
        char b[400];
        snprintf(b, sizeof(b), "GEARHIDE:   #%zu%s handle 0x%llx, %u components, %s [%s] names: %s", i + 1,
                 (mode == 1 || mode - 2 == static_cast<int>(i)) ? " (HIDE)" : "", static_cast<unsigned long long>(kids[i].handle),
                 kids[i].components, kids[i].eventPath ? "hide event" : "drawable", kids[i].classes.c_str(), kids[i].names.c_str());
        modlog::Line(b);
    }
}

// ---- RETICLE watch (2026-10-01, rope arrows) ----
// Rope arrows at range: with head aim on the rope-arrow reticle flickers on and off while charging and a normal arrow
// flies. The game's reticle target (FUN_1409bf230 -> FUN_1409b9ae0, every frame while aiming) is a ray along the camera
// component's aim record (+0x450, or +0x930 while stack-2 weight +0xC70 > 0 -- the records head aim turns); the rope
// target (+0xC28 handle, +0xC30 point) is the candidate object's rope markup within its radius of that ray
// (FUN_1406e7e40). All in the player's targeting object T = *(Lara's component [*(u32*)0x1416B99C8] + 0x2A8):
// +0x1B00 ray origin, +0x1B10 hit point (or ray end), +0xBF8 object hit, +0xC00 hit distance. This logs, while aiming,
// every change of "rope target present" with the frame's AER eye, the ray direction and the stack-2 weight, and a 1 s
// summary -- to see what alternates.
namespace {
struct RetState { uint64_t rope = 0, hit = 0; uint32_t type = 0; float yaw = 0, pitch = 0, dist = 0, w2 = 0, org[3] = {}; bool ok = false; };

bool ReadRet(uintptr_t camComp, RetState* r) {
    uint32_t idx = 0;
    if (!Rd(Exe() + 0x16B99C8, &idx) || idx > 512) return false;
    const uintptr_t e = Entry(LaraHandle());
    if (!e) return false;
    const uintptr_t comp = Component(e, idx);
    uintptr_t T = 0;
    if (!comp || !Rd(comp + 0x2A8, &T) || !T) return false;
    float o[3], h[3];
    for (int k = 0; k < 3; ++k) if (!Rd(T + 0x1B00 + 4 * k, &o[k]) || !Rd(T + 0x1B10 + 4 * k, &h[k])) return false;
    Rd(T + 0xC28, &r->rope);
    Rd(T + 0xBF8, &r->hit);
    Rd(T + 0xC00, &r->dist);
    Rd(T + 0xC04, &r->type);
    for (int k = 0; k < 3; ++k) r->org[k] = o[k];
    if (camComp) Rd(camComp + 0xC70, &r->w2);
    const float dx = h[0] - o[0], dy = h[1] - o[1], dz = h[2] - o[2];
    r->yaw = std::atan2(-dx, dy) * 57.29578f;
    r->pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.29578f;
    r->ok = true;
    return true;
}

std::atomic<uint32_t> g_retFrames{0}, g_retRope[2], g_retFrameEye[2], g_retChanges{0};
std::atomic<int> g_retYawMin{0}, g_retYawMax{0};
std::atomic<bool> g_retActive{false};
}

void ReticleWatchGameThread(uint64_t gameFrame, uintptr_t cameraComponent) {
    static bool wasAiming = false, lastRope = false;
    static int logged = 0;
    const bool aiming = CameraAimingNow();
    if (!aiming) { wasAiming = false; return; }
    RetState r;
    if (!ReadRet(cameraComponent, &r)) return;
    const int eye = static_cast<int>(gameFrame & 1);
    const bool rope = r.rope != 0;
    g_retActive.store(true, std::memory_order_relaxed);
    if (g_retFrames.fetch_add(1, std::memory_order_relaxed) == 0) {
        g_retYawMin.store(static_cast<int>(r.yaw * 100), std::memory_order_relaxed);
        g_retYawMax.store(static_cast<int>(r.yaw * 100), std::memory_order_relaxed);
    }
    const int y100 = static_cast<int>(r.yaw * 100);
    if (y100 < g_retYawMin.load(std::memory_order_relaxed)) g_retYawMin.store(y100, std::memory_order_relaxed);
    if (y100 > g_retYawMax.load(std::memory_order_relaxed)) g_retYawMax.store(y100, std::memory_order_relaxed);
    g_retFrameEye[eye].fetch_add(1, std::memory_order_relaxed);
    if (rope) g_retRope[eye].fetch_add(1, std::memory_order_relaxed);
    if (wasAiming && rope != lastRope) {
        g_retChanges.fetch_add(1, std::memory_order_relaxed);
        if (logged < 300) {
            ++logged;
            char b[260];
            snprintf(b, sizeof(b), "RETICLE: frame %llu (eye %s) rope target %s | ray yaw %.2f pitch %.2f deg, hit %.1f m, object 0x%llx | stack-2 weight %.2f | %s",
                     static_cast<unsigned long long>(gameFrame), eye ? "R" : "L", rope ? "ON" : "off", r.yaw, r.pitch, r.dist / 100.0f,
                     static_cast<unsigned long long>(r.hit), r.w2, IsFirstPersonEnabled() ? "first person" : "third person");
            modlog::Line(b);
        }
    }
    wasAiming = true;
    lastRope = rope;
    // Frame-by-frame sequence (first write of each game frame): eye, hit object's table slot, its type (+0xC04, what
    // the reticle node reads), hit distance, pitch, and how far the ray origin moved since the previous frame.
    static uint64_t lastFrame = 0;
    static float lastOrg[3] = {};
    static std::string seq;
    static int seqN = 0, seqLines = 0;
    if (gameFrame != lastFrame) {
        lastFrame = gameFrame;
        const float dx = r.org[0] - lastOrg[0], dy = r.org[1] - lastOrg[1], dz = r.org[2] - lastOrg[2];
        for (int k = 0; k < 3; ++k) lastOrg[k] = r.org[k];
        // Head pitch relative to the game camera (the d head aim injects): forward of d = (-sin y cos p, cos y cos p, sin p).
        float d[4] = { 0, 0, 0, 1 };
        float headPitch = 0.0f;
        if (AerHeadAimDelta(d)) {
            const float fz = 2.0f * (d[1] * d[2] + d[3] * d[0]);
            headPitch = std::asin(fz < -1.0f ? -1.0f : (fz > 1.0f ? 1.0f : fz)) * 57.29578f;
        }
        char f[112];
        snprintf(f, sizeof(f), " %s:%u/%d/%.1f/%.1f/%.1f/%.0f", eye ? "R" : "L", static_cast<uint32_t>(r.hit) & 0xFFFF, static_cast<int>(r.type),
                 r.dist / 100.0f, r.pitch, headPitch, std::sqrt(dx * dx + dy * dy + dz * dz));
        seq += f;
        if (++seqN >= 36) {
            if (seqLines < 4000) { ++seqLines; modlog::Line("RETSEQ (eye:object slot/type/hit m/ray pitch/head pitch deg/origin step cm):" + seq); }
            seq.clear();
            seqN = 0;
        }
    }
}

void ReticleWatchOnPresent() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    if (!g_retActive.exchange(false)) return;
    const uint32_t n = g_retFrames.exchange(0);
    if (!n) return;
    char b[260];
    snprintf(b, sizeof(b), "RETICLE (1 s aiming): %u frames, rope target in %u of %u left-eye frames and %u of %u right-eye frames, %u changes | ray yaw %.2f..%.2f deg",
             n, g_retRope[0].exchange(0), g_retFrameEye[0].exchange(0), g_retRope[1].exchange(0), g_retFrameEye[1].exchange(0),
             g_retChanges.exchange(0), g_retYawMin.load() / 100.0f, g_retYawMax.load() / 100.0f);
    modlog::Line(b);
}

}
