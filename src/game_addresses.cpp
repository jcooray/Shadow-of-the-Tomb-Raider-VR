#include "game_addresses.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef GAMEADDR_TEST   // tools/gameaddr_test.cpp: the resolver run on SOTTR.exe mapped as an image, outside the game
#include <iostream>
namespace modlog { inline void Line(const std::string& s) { std::cout << s << std::endl; } }
namespace vrmod { uintptr_t g_testImage = 0; }
#else
#include "logger.h"
#endif

namespace vrmod {
namespace {

// Patterns: ?? = any byte. `at` = offset of the target in the pattern. For data: the pattern starts at an instruction
// referencing the global with a RIP-relative disp32 at `disp` and total length `len`; the global = insn + len + disp32.
// Relative branch targets and RIP displacements are wildcarded; struct offsets and immediates are kept.
struct Sig {
    GameAddr id;
    const char* name;
    uint32_t knownRva;   // in the build the patterns were made from (for the log only)
    const char* pattern;
    int at;
    int disp, len;       // data patterns only (len 0 = code)
};
const Sig kSigs[] = {
    { GameAddr::PostEvent, "event post", 0x8E75B0, "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 50 48 8B F1", 0, 0, 0 },
    { GameAddr::DlssEvaluate, "DLSS evaluate", 0x101B860,
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC A0 01 00 00 4C 8D 71 70 48 8B F1 49 8B CE "
      "49 8B F9 49 8B D8 48 8B EA FF 15 ?? ?? ?? ?? 33 D2 48 8D 4C 24 30 41 B8 68 01 00 00 E8 ?? ?? ?? ?? 48 8B 84 24 E0 01 "
      "00 00 4C 8D 4C 24 30 F3 0F 10 8C 24 00 02 00 00 48 8B CD F3 0F 10 84 24 F8 01 00 00 0F 57 0D ?? ?? ?? ?? 8B 94 24 "
      "10 02 00 00", 0, 0, 0 },
    { GameAddr::DlssCreate, "DLSS create", 0x101B9C0, "48 8B C4 48 89 78 20 41 54 41 56 41 57 48 83 EC 50", 0, 0, 0 },
    { GameAddr::DlssEnsure, "DLSS ensure", 0x101D140, "44 89 4C 24 20 44 89 44 24 18 48 89 54 24 10 53 56", 0, 0, 0 },
    { GameAddr::DlssCreateHelper, "DLSS create helper", 0x101C3D0,
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 7C 24 50 41 8B D8", 0, 0, 0 },
    { GameAddr::GameStateFn, "game state", 0x64BF60, "48 8B 05 ?? ?? ?? ?? C3 ?? ?? ?? ?? ?? ?? ?? ?? 48 63 C2", 0, 0, 0 },
    { GameAddr::MoviesFn, "movies", 0x224E00, "48 83 EC 28 80 79 01 00 75 ?? 48 8B 49 10", 0, 0, 0 },
    { GameAddr::RecordCopyFn, "record copy", 0x164300,
      "0F 28 02 66 0F 7F 01 0F 28 4A 10 66 0F 7F 49 10 0F 28 42 20 66 0F 7F 41 20 0F 28 4A 30 66 0F 7F 49 30 0F 28 42 40", 0, 0, 0 },
    { GameAddr::AimRet0, "aim return 0", 0x168233, "48 8B 9D 28 02 00 00 0F 28 B5 D0 01 00 00", 0, 0, 0 },
    { GameAddr::AimRet1, "aim return 1", 0x168213, "48 8B 96 78 0C 00 00 48 8D 4D 10 E8 ?? ?? ?? ??", 0, 0, 0 },
    { GameAddr::AimRet2, "aim return 2", 0x1687A3, "83 BB 90 0F 00 00 00 75 ?? 48 8B 93 78 0C 00 00", 0, 0, 0 },
    { GameAddr::AimRet3, "aim return 3", 0x1687CC, "F3 0F 5C F7 F3 0F 11 B3 70 0C 00 00", 0, 0, 0 },
    { GameAddr::PrevCopyRet0, "previous-record copy 0", 0x167E42, "44 8B AE 18 0F 00 00 49 BF F0 FF FF FF FF FF FF 0F", 0, 0, 0 },
    { GameAddr::PrevCopyRet1, "previous-record copy 1", 0x1682DA, "44 8B BB 90 0F 00 00 8B 93 30 0F 00 00", 0, 0, 0 },
    { GameAddr::MenusGlobal, "menus object", 0x146BF38,
      "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 48 8B 10 FF 52 38 8B 04 3E", 0, 3, 7 },
    { GameAddr::MoviesGlobal, "movie player", 0x1F3AAE0,
      "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? "
      "?? ?? 48 8B C8 E8 ?? ?? ?? ??", 0, 3, 7 },
    // Body turn (body_turn.cpp; 2026-10-18 from tools/make_sigs.py): a LEA of the pool while walking its entries
    // (+0x110, +0xD0, +0xD8), and the index read just before *(*(inst + 0xD8) + index * 8) + 0x2A8 -- the player check.
    { GameAddr::InstancePool, "world-object pool", 0x301D0C0,
      "48 8D 15 ?? ?? ?? ?? 48 39 8C 10 10 01 00 00 75 ?? 83 BC 10 D0 00 00 00 03 76 ?? 48 8B 84 10 D8 00 00 00 48 8B 48 18 "
      "EB ?? 48 8B CE 48 85 C9 74 ?? 48 8D 55 0B", 0, 3, 7 },
    { GameAddr::PlayerIndexGlobal, "player slot index", 0x16B99C8,
      "8B 0D ?? ?? ?? ?? 48 8B 83 D8 00 00 00 48 8B 0C C8 4C 8B B9 A8 02 00 00 EB ??", 0, 2, 6 },
};
static_assert(sizeof(kSigs) / sizeof(kSigs[0]) == static_cast<size_t>(GameAddr::Count), "one pattern per address");

struct Section { const uint8_t* p = nullptr; size_t n = 0; };
uintptr_t g_base = 0;
Section g_text, g_rdata;
uintptr_t g_addr[static_cast<int>(GameAddr::Count)] = {};
std::once_flag g_once;
std::mutex g_eventMutex;
std::unordered_map<std::string, uintptr_t> g_events;

void FindSections() {
#ifdef GAMEADDR_TEST
    g_base = g_testImage;
#else
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
#endif
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        char name[9] = {};
        memcpy(name, s->Name, 8);
        Section sec{ reinterpret_cast<const uint8_t*>(g_base + s->VirtualAddress), (std::max)(s->Misc.VirtualSize, s->SizeOfRawData) };
        if (!strcmp(name, ".text")) g_text = sec;
        else if (!strcmp(name, ".rdata")) g_rdata = sec;
    }
}

std::vector<int> Parse(const char* pat) {
    std::vector<int> out;
    for (const char* c = pat; *c;) {
        if (*c == ' ') { ++c; continue; }
        if (c[0] == '?') { out.push_back(-1); c += 2; continue; }
        unsigned v = 0;
        sscanf_s(c, "%2x", &v);
        out.push_back(static_cast<int>(v));
        c += 2;
    }
    return out;
}

// Matches of `pat` in the section (at most 2: we only need to know if it is unique).
int Find(const Section& sec, const std::vector<int>& pat, const uint8_t** first) {
    int n = 0;
    if (pat.empty() || pat[0] < 0 || sec.n < pat.size()) return 0;
    const uint8_t* p = sec.p;
    const uint8_t* end = sec.p + sec.n - pat.size();
    while (p <= end) {
        p = static_cast<const uint8_t*>(memchr(p, pat[0], static_cast<size_t>(end - p) + 1));
        if (!p) break;
        size_t k = 1;
        while (k < pat.size() && (pat[k] < 0 || p[k] == pat[k])) ++k;
        if (k == pat.size()) {
            if (n++ == 0) *first = p;
            if (n > 1) break;
        }
        ++p;
    }
    return n;
}

int g_foundCount = 0, g_eventsOk = 0, g_eventsTotal = 0;
bool g_knownBuild = false;

void ResolveAll() {
    FindSections();
    if (!g_text.p) { modlog::Line("GAMEADDR: no .text section -- every game hook off."); return; }
    int found = 0, moved = 0;
    std::ostringstream log;
    for (const Sig& s : kSigs) {
        const uint8_t* hit = nullptr;
        const int n = Find(g_text, Parse(s.pattern), &hit);
        uintptr_t a = 0;
        if (n == 1) {
            a = reinterpret_cast<uintptr_t>(hit) + s.at;
            if (s.len) {
                int32_t d;
                memcpy(&d, hit + s.disp, 4);
                a = reinterpret_cast<uintptr_t>(hit) + s.len + d;
            }
        }
        g_addr[static_cast<int>(s.id)] = a;
        if (a) {
            ++found;
            if (a - g_base != s.knownRva) ++moved;
        } else {
            log << "\n  " << s.name << ": pattern " << (n == 0 ? "not found" : "found more than once") << " -- its feature is off";
        }
    }
    // Which build: the exe's link timestamp (the patterns were made from the Steam build linked 2023-07-10, 0x64AC5C46).
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    const DWORD stamp = nt->FileHeader.TimeDateStamp;
    char st[160];
    snprintf(st, sizeof(st), "GAMEADDR: SOTTR.exe linked 0x%08lX, image 0x%lX bytes -- %s", static_cast<unsigned long>(stamp),
             static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage), stamp == 0x64AC5C46 ? "the build the mod was made for" : "a DIFFERENT build");
    modlog::Line(st);
    g_foundCount = found;
    g_knownBuild = stamp == 0x64AC5C46;
    char b[200];
    snprintf(b, sizeof(b), "GAMEADDR: %d of %d game addresses found by pattern%s", found, static_cast<int>(GameAddr::Count),
             moved ? " (some moved: a different game build than the patterns were made from)" : " (all at their known places: the known game build)");
    modlog::Line(b + log.str());
}

// Event id globals: name string (.rdata) <- LEA r64,[rip+d32] (.text) -> the register call (E8) after it -> the first
// MOV [rip+d32], r32 after that call. One pass over .text for all requested names.
void ScanEvents(const std::vector<std::string>& names) {
    std::unordered_map<uintptr_t, std::string> strAt;   // string address -> name
    for (const std::string& nm : names) {
        const size_t len = nm.size() + 1;
        for (size_t i = 1; g_rdata.p && i + len <= g_rdata.n; ++i) {
            if (g_rdata.p[i] == static_cast<uint8_t>(nm[0]) && g_rdata.p[i - 1] == 0 && !memcmp(g_rdata.p + i, nm.c_str(), len)) {
                strAt[reinterpret_cast<uintptr_t>(g_rdata.p + i)] = nm;
            }
        }
    }
    std::unordered_map<std::string, uintptr_t> found;
    std::unordered_map<std::string, bool> ambiguous;
    const uint8_t* t = g_text.p;
    for (size_t k = 0; k + 7 + 48 + 5 + 32 + 6 < g_text.n; ++k) {
        if ((t[k] != 0x48 && t[k] != 0x4C) || t[k + 1] != 0x8D || (t[k + 2] & 0xC7) != 0x05) continue;
        int32_t d;
        memcpy(&d, t + k + 3, 4);
        auto it = strAt.find(reinterpret_cast<uintptr_t>(t + k + 7) + d);
        if (it == strAt.end()) continue;
        size_t c = k + 7;
        while (c < k + 7 + 48 && t[c] != 0xE8) ++c;
        if (t[c] != 0xE8) continue;
        for (size_t j = c + 5; j < c + 5 + 32; ++j) {
            if (t[j] == 0x89 && (t[j + 1] & 0xC7) == 0x05) {
                int32_t d2;
                memcpy(&d2, t + j + 2, 4);
                const uintptr_t g = reinterpret_cast<uintptr_t>(t + j + 6) + d2;
                auto f = found.find(it->second);
                if (f != found.end() && f->second != g) ambiguous[it->second] = true;
                found[it->second] = g;
                break;
            }
        }
    }
    for (const std::string& nm : names) g_events[nm] = ambiguous.count(nm) ? 0 : (found.count(nm) ? found[nm] : 0);
}

}  // namespace

void GameAddressSummary(int* found, int* total, int* eventsFound, int* eventsTotal, bool* knownBuild) {
    std::call_once(g_once, ResolveAll);
    *found = g_foundCount;
    *total = static_cast<int>(GameAddr::Count);
    std::lock_guard<std::mutex> lock(g_eventMutex);
    *eventsFound = g_eventsOk;
    *eventsTotal = g_eventsTotal;
    *knownBuild = g_knownBuild;
}

uintptr_t GameAddress(GameAddr which) {
    std::call_once(g_once, ResolveAll);
    return g_addr[static_cast<int>(which)];
}

uintptr_t GameEventIdAddress(const char* name) {
    std::call_once(g_once, ResolveAll);
    std::lock_guard<std::mutex> lock(g_eventMutex);
    auto it = g_events.find(name);
    return it != g_events.end() ? it->second : 0;
}

// Batch form used by the cinematic watch: resolves all its names in one .text pass.
void GameEventIdsResolve(const char* const* names, int n) {
    std::call_once(g_once, ResolveAll);
    std::vector<std::string> v(names, names + n);
    std::lock_guard<std::mutex> lock(g_eventMutex);
    ScanEvents(v);
    int ok = 0;
    for (const std::string& s : v) ok += g_events[s] != 0;
    g_eventsOk = ok;
    g_eventsTotal = n;
    modlog::Line("GAMEADDR: " + std::to_string(ok) + " of " + std::to_string(n) + " event ids found from their names");
}

}  // namespace vrmod
