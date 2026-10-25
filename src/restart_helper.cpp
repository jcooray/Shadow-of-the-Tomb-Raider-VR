// SOTTR-VR-restart.exe: starts the game again once it has closed (the mod's first run: once a level has loaded, its
// "Restart the game" notice -> A / Enter, then the game's own quit message, so the render resolution chosen for the PC
// applies). Started by the mod as it asks the game to close; no window of its own.
//
// Usage: SOTTR-VR-restart.exe <pid> <what to start> [its arguments...]
//   <pid>           the game's process: waited for (up to 3 minutes; none started if it is still running), then 2 s
//                   more (Steam marks the game stopped)
//   <what to start> a URL (steam://rungameid/750920) or the game's exe; the rest of the line is passed on as its
//                   arguments (an Epic / GOG start's own command line)
#include <windows.h>
#include <shellapi.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) return 1;
    const DWORD pid = static_cast<DWORD>(wcstoul(argv[1], nullptr, 10));
    std::wstring target = argv[2], args;
    for (int i = 3; i < argc; ++i) {
        if (!args.empty()) args += L' ';
        const std::wstring a = argv[i];
        args += a.find_first_of(L" \t\"") == std::wstring::npos ? a : L"\"" + a + L"\"";
    }
    // In a level the game asks to confirm its quit message first; if the player says no, it keeps running and this gives
    // up after 3 minutes without starting another.
    if (HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        const DWORD r = WaitForSingleObject(game, 180000);
        CloseHandle(game);
        if (r != WAIT_OBJECT_0) {
            LocalFree(argv);
            return 2;
        }
    }
    Sleep(2000);
    wchar_t dir[MAX_PATH] = {};
    std::wstring workDir;
    if (target.find(L"://") == std::wstring::npos) {   // an exe: start it in its own folder
        wcsncpy_s(dir, target.c_str(), _TRUNCATE);
        if (wchar_t* slash = wcsrchr(dir, L'\\')) { *slash = 0; workDir = dir; }
    }
    ShellExecuteW(nullptr, L"open", target.c_str(), args.empty() ? nullptr : args.c_str(), workDir.empty() ? nullptr : workDir.c_str(),
                  SW_SHOWNORMAL);
    LocalFree(argv);
    return 0;
}
