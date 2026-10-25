// Offline test of src/game_addresses.cpp: maps SOTTR.exe as an image (read-only, nothing runs) and resolves every
// pattern and event id, comparing with the addresses of the build the patterns were made from.
// Usage: vrmod_gameaddr_test "<game folder>\SOTTR.exe" (default: the Steam library path of the developer's machine).
#include <windows.h>
#include <cstdio>
#include "../src/game_addresses.h"
namespace vrmod { extern uintptr_t g_testImage; }
int main(int argc, char** argv) {
    wchar_t arg[MAX_PATH]{};
    if (argc > 1) MultiByteToWideChar(CP_ACP, 0, argv[1], -1, arg, MAX_PATH);
    const wchar_t* exe = argc > 1 ? arg : L"D:/SteamLibrary/steamapps/common/Shadow of the Tomb Raider/SOTTR.exe";
    HMODULE m = LoadLibraryExW(exe, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) { printf("map failed %lu\n", GetLastError()); return 1; }
    vrmod::g_testImage = reinterpret_cast<uintptr_t>(m) & ~uintptr_t(3);   // the low bits flag a resource mapping
    int bad = 0;
    for (int i = 0; i < static_cast<int>(vrmod::GameAddr::Count); ++i) {
        const uintptr_t a = vrmod::GameAddress(static_cast<vrmod::GameAddr>(i));
        printf("addr %2d -> %s 0x%llX\n", i, a ? "RVA" : "MISSING", a ? (unsigned long long)(a - vrmod::g_testImage) : 0ull);
        bad += a == 0;
    }
    const char* names[] = { "evCinematicStarted", "evCinematicFinished", "lc2_MainMenu2Game", "evMenuStateChanged", "OnInteractStart" };
    vrmod::GameEventIdsResolve(names, 5);
    for (const char* n : names) {
        const uintptr_t a = vrmod::GameEventIdAddress(n);
        printf("event %-22s -> RVA 0x%llX\n", n, a ? (unsigned long long)(a - vrmod::g_testImage) : 0ull);
        bad += a == 0;
    }
    printf("%s\n", bad ? "FAIL" : "ALL FOUND");
    return bad ? 1 : 0;
}
