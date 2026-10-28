#include "cheeky_loader.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <string>

#include "logger.h"
#include "settings.h"

namespace vrmod {

namespace {

std::atomic<HANDLE> g_worker{ nullptr };
std::atomic<int> g_state{ 0 };   // 0 not started, 1 starting, 2 started, 3 not loaded / failed
std::atomic<bool> g_waitTimedOut{ false };
thread_local bool t_inWorker = false;

std::wstring GameDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    return dir.substr(0, dir.find_last_of(L'\\') + 1);
}

// An earlier install's Cheeky loader (version.dll with its CheekyBootstrap_Status export, stored as plain text) still
// beside the game: it loads Cheeky itself.
bool OldLoaderPresent(const std::wstring& dir) {
    std::ifstream in(dir + L"version.dll", std::ios::binary);
    if (!in) return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes.find("CheekyBootstrap_Status") != std::string::npos;
}

DWORD WINAPI Worker(void*) {
    t_inWorker = true;
    const std::wstring dir = GameDir();
    const std::wstring host = dir + L"CheekyFoveatedDLSS\\CheekyFoveatedDLSSHost.dll";
    if (GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES) {   // foveated rendering not installed
        g_state.store(3);
        return 0;
    }
    if (SettingGet("foveation.mode") < 1.0f) {
        modlog::Line("CHEEKY: foveated rendering is off -- CheekyFoveatedDLSS not loaded");
        g_state.store(3);
        return 0;
    }
    if (OldLoaderPresent(dir)) {
        modlog::Line("CHEEKY: an earlier install's version.dll loads CheekyFoveatedDLSS -- not loaded twice (run Setup to tidy it)");
        g_state.store(3);
        return 0;
    }
    // No current-directory or PATH search: the host and its private dependencies live in its own folder.
    const HMODULE h = LoadLibraryExW(host.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) {
        modlog::Line("CHEEKY: could not load CheekyFoveatedDLSSHost.dll (error " + std::to_string(GetLastError()) + ")");
        g_state.store(3);
        return 0;
    }
    using StartFn = bool (*)(unsigned);
    const auto start = reinterpret_cast<StartFn>(GetProcAddress(h, "CheekyHost_Start"));
    const bool ok = start && start(1u);   // 1 = standalone host (Cheeky's CheekyRuntimeHost)
    modlog::Line(ok ? "CHEEKY: CheekyFoveatedDLSS started (loaded by this dxgi.dll)"
                    : "CHEEKY: CheekyFoveatedDLSS didn't start -- see its host log (Foveated rendering log, Support page)");
    g_state.store(ok ? 2 : 3);   // the module stays loaded either way: it may have hooked something already
    return 0;
}

}  // namespace

void CheekyLoaderStart() {
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1)) return;
    const HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!h) {
        g_state.store(3);
        return;
    }
    g_worker.store(h);
}

void CheekyLoaderAfterFactory() {
    if (t_inWorker || g_state.load() != 1 || g_waitTimedOut.load()) return;
    const HANDLE h = g_worker.load();
    if (h && WaitForSingleObject(h, 10000) == WAIT_TIMEOUT) {
        g_waitTimedOut.store(true);
        modlog::Line("CHEEKY: CheekyFoveatedDLSS still starting after 10 s -- the game goes on without waiting");
    }
}

}  // namespace vrmod
