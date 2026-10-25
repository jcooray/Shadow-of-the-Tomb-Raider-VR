#pragma once

#include <cstdint>

namespace vrmod {

// Game code and data the mod hooks or reads, found by byte pattern at startup instead of fixed addresses, so a game
// update that moves code doesn't put a hook in the wrong place: a pattern that doesn't match exactly once leaves its
// address 0 and the feature using it switches itself off (docs/RELEASE.md). Patterns come from tools/make_sigs.py and
// were checked to match exactly once in the Steam build's .text (2026-10-07).
enum class GameAddr {
    PostEvent,          // event-post function (cutscene / traversal events)
    DlssEvaluate,       // the game's DLSS wrapper: evaluate, create, ensure, and its NGX create helper
    DlssCreate,
    DlssEnsure,
    DlssCreateHelper,
    GameStateFn,        // returns the game-state value (menus / loading)
    MoviesFn,           // non-zero while a video plays
    RecordCopyFn,       // the camera blend stacks' record copy (holds the camera write block)
    AimRet0,            // return addresses of the blend stacks' record-copy calls (head aim)
    AimRet1,
    AimRet2,
    AimRet3,
    PrevCopyRet0,
    PrevCopyRet1,
    MenusGlobal,        // pointer to the object whose +0x7A8 is >= 0 while a menu is open
    MoviesGlobal,       // the movie player argument for MoviesFn
    InstancePool,       // the static world-object pool (0x120-byte entries; Lara is one of them) -- body turn
    PlayerIndexGlobal,  // the index of the player's slot in an instance's +0xD8 table -- body turn
    Count
};

// Absolute address in the running game, or 0 if its pattern wasn't found exactly once. Resolves everything on first use.
uintptr_t GameAddress(GameAddr which);
// Event id globals: GameEventIdsResolve finds, in one pass, the globals the game stores the named events' ids in (name
// string -> the code registering it -> its store); GameEventIdAddress then returns one, or 0 if not found exactly once.
void GameEventIdsResolve(const char* const* names, int n);
uintptr_t GameEventIdAddress(const char* name);
// For the overlay's Status page: addresses found / total, event ids found / total, and whether SOTTR.exe is the build
// the patterns were made from.
void GameAddressSummary(int* found, int* total, int* eventsFound, int* eventsTotal, bool* knownBuild);

}  // namespace vrmod
