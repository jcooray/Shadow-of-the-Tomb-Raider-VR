#include "crt_shim.h"

#include <windows.h>
#include <MinHook.h>

#include <cstring>
#include <string>

#include "logger.h"
#include "settings.h"

#pragma comment(lib, "version.lib")

// The game's own old C++ runtime against newer OpenXR software (2026-11-18, test build 1.1.1-ik107).
//
// Shadow of the Tomb Raider keeps a msvcp140.dll of its own beside the exe (14.28, 2021), and Windows gives every DLL
// loaded into the process that one -- whatever newer one the machine has. A DLL built with Visual Studio 2022 17.10 or
// later constructs std::mutex at compile time (a zeroed object, no _Mtx_init_in_situ call); the old runtime's _Mtx_lock
// reaches the lock through a table pointer its own initialiser would have set -- null in such an object -- and the
// process dies reading address 0 at MSVCP140.dll+0x12c10. Seen twice, in OpenXR software the game loads and this mod
// can do nothing about otherwise:
//   - Grumpy John (Flat2VR thread, 2026-11-16): Pimax Play updated, PiOpenXR_64.dll the caller -- no VR at all;
//   - the user (2026-11-18): an API layer another game had installed five minutes earlier
//     (XR_APILAYER_NOVENDOR_neurorunner), loaded into every OpenXR app.
// The object's layout is the same in both runtimes (the newer one calls the table pointer's slot "unused" and works on
// the lock beside it), so the newer runtime's functions work on either kind of object, the old one's only on its own.
// So: where the process runs on a msvcp140.dll older than 14.40 and the machine has a newer one in System32, that one
// is loaded as well (by its full path: a second module) and every _Mtx_* and _Cnd_* function of the old one is sent
// to the newer one's. Before the OpenXR instance is made. [debug] crt_shim=0 leaves the runtime alone.
//
// All of them at once (2026-11-22, 1.1.3): they were redirected one after another, each with the other threads stopped
// and started again, and for those few milliseconds the process ran on a mixture -- a mutex the game made then was
// initialised by the newer runtime (_Mtx_init_in_situ, redirected already: no table pointer) and unlocked by the old
// one (_Mtx_unlock, the last name in the list, not yet redirected), which died reading address 0x18 at
// MSVCP140.dll+0x12f55. Two starts in a row closed to the desktop that way. The redirections are now queued and applied
// in one step, the other threads stopped once: no thread ever sees some of them without the others.
namespace vrmod {
namespace {

bool FileVersion(const wchar_t* path, unsigned* major, unsigned* minor) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return false;
    std::string buf(size, '\0');
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path, 0, size, &buf[0]) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return false;
    *major = HIWORD(info->dwFileVersionMS);
    *minor = LOWORD(info->dwFileVersionMS);
    return true;
}

std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s += static_cast<char>(*w < 128 ? *w : '?');
    return s;
}

}  // namespace

void CrtMutexShim() {
    static bool done = false;
    if (done) return;
    done = true;
    if (SettingGet("debug.crt_shim") == 0.0f) return;
    const HMODULE oldMod = GetModuleHandleW(L"msvcp140.dll");
    if (!oldMod) return;   // (nothing in the process uses it yet: whatever loads later gets the machine's own)
    wchar_t oldPath[MAX_PATH] = {}, sysDir[MAX_PATH] = {};
    unsigned maj = 0, min = 0, newMaj = 0, newMin = 0;
    if (!GetModuleFileNameW(oldMod, oldPath, MAX_PATH) || !FileVersion(oldPath, &maj, &min)) return;
    if (maj > 14 || (maj == 14 && min >= 40)) return;   // (new enough: compile-time mutexes work)
    if (!GetSystemDirectoryW(sysDir, MAX_PATH)) return;
    const std::wstring newPath = std::wstring(sysDir) + L"\\msvcp140.dll";
    char line[600];
    if (_wcsicmp(newPath.c_str(), oldPath) == 0 || !FileVersion(newPath.c_str(), &newMaj, &newMin) || newMaj < 14 || (newMaj == 14 && newMin < 40)) {
        snprintf(line, sizeof(line), "CRT: the process runs on msvcp140.dll %u.%u (%s) and the machine has none newer than 14.40 -- OpenXR software built with a "
                 "newer Visual Studio can crash in it (reading address 0 in MSVCP140.dll). Installing the current Visual C++ redistributable lets this mod "
                 "work around it.", maj, min, Narrow(oldPath).c_str());
        modlog::Line(line);
        return;
    }
    const HMODULE newMod = LoadLibraryExW(newPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!newMod || newMod == oldMod) {
        snprintf(line, sizeof(line), "CRT: the process runs on msvcp140.dll %u.%u (%s); the machine's %u.%u could not be loaded beside it (error %lu) -- left "
                 "as it is", maj, min, Narrow(oldPath).c_str(), newMaj, newMin, GetLastError());
        modlog::Line(line);
        return;
    }
    // Every _Mtx_* / _Cnd_* export of the old one -> the newer one's of the same name.
    const BYTE* base = reinterpret_cast<const BYTE*>(oldMod);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return;
    const IMAGE_EXPORT_DIRECTORY* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const DWORD* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    const WORD* ords = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
    const DWORD* funcs = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
    int sent = 0, missing = 0, failed = 0;
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (strncmp(name, "_Mtx_", 5) != 0 && strncmp(name, "_Cnd_", 5) != 0) continue;
        const DWORD rva = funcs[ords[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;   // (a forwarder: not code)
        void* oldFn = const_cast<BYTE*>(base) + rva;
        void* newFn = reinterpret_cast<void*>(GetProcAddress(newMod, name));
        if (!newFn) { ++missing; continue; }
        if (MH_CreateHook(oldFn, newFn, nullptr) == MH_OK && MH_QueueEnableHook(oldFn) == MH_OK) ++sent;
        else ++failed;
    }
    if (sent && MH_ApplyQueued() != MH_OK) {   // (all of them in one step: see the note above)
        failed += sent;
        sent = 0;
    }
    snprintf(line, sizeof(line), "CRT: the process runs on the game's own msvcp140.dll %u.%u (%s), too old for OpenXR software built with a newer Visual "
             "Studio (it crashes reading address 0 in MSVCP140.dll). The machine's %u.%u is loaded beside it and %d of the old one's mutex and condition "
             "functions go to it now (%d not in the newer one, %d not redirected). [debug] crt_shim=0 turns this off.", maj, min, Narrow(oldPath).c_str(),
             newMaj, newMin, sent, missing, failed);
    modlog::Line(line);
}

}  // namespace vrmod
