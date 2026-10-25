#include "logger.h"

#include <windows.h>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace modlog {

namespace {
std::ofstream g_file;
std::mutex g_mutex;
bool g_closed = false;

std::wstring GetSelfDirectory() {
    HMODULE hSelf = nullptr;
    // GetModuleHandleExW with the address-of-this-function flag finds the
    // module that contains this code, i.e. our own DLL, regardless of name.
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetSelfDirectory),
        &hSelf);

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(hSelf, path, MAX_PATH);

    std::wstring full(path);
    size_t slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : full.substr(0, slash);
}
} // namespace

// Opens (truncating) the log the first time anything needs it. Init() and
// Line() both go through this, so it no longer matters which thread gets
// there first -- the game's own thread can now log from inside the DXGI export
// wrappers before the init thread has even started -- and a later Init() can't
// truncate lines that were already written. Caller holds g_mutex.
void OpenOnceLocked() {
    if (g_file.is_open() || g_closed) {
        return;
    }
    std::wstring dir = GetSelfDirectory();
    std::wstring logPath = dir + L"\\vrmod.log";
    // Keep the last three sessions (vrmod.prev1.log newest) instead of truncating them away.
    DeleteFileW((dir + L"\\vrmod.prev3.log").c_str());
    MoveFileExW((dir + L"\\vrmod.prev2.log").c_str(), (dir + L"\\vrmod.prev3.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW((dir + L"\\vrmod.prev1.log").c_str(), (dir + L"\\vrmod.prev2.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(logPath.c_str(), (dir + L"\\vrmod.prev1.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file.open(logPath, std::ios::out | std::ios::trunc);
}

void Init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    OpenOnceLocked();
}

void Close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_closed = true;
    if (g_file.is_open()) {
        g_file.close();
    }
}

void Line(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    OpenOnceLocked();
    if (!g_file.is_open()) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);

    g_file << std::put_time(&tmBuf, "%H:%M:%S") << "  " << message << std::endl;
    g_file.flush();
}

} // namespace modlog
