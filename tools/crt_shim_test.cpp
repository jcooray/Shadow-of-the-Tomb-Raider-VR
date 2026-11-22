// Offline check of src/crt_shim.cpp: the crash of a compile-time std::mutex in an old msvcp140.dll, and the shim that
// cures it. Nothing of the game runs; its own old msvcp140.dll is loaded from the path given.
// Usage: vrmod_crt_shim_test "<game folder>\msvcp140.dll"
#include <windows.h>
#include <MinHook.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../src/crt_shim.h"

namespace modlog {
void Line(const std::string& message) { printf("log: %s\n", message.c_str()); }
}  // namespace modlog
namespace vrmod {
float SettingGet(const char*) { return 1.0f; }
}  // namespace vrmod

namespace {
int g_failed = 0;
void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_failed;
}

using MtxFn = int (*)(void*);
using MtxVoid = void (*)(void*);
using MtxInit = void (*)(void*, int);

// A std::mutex as a DLL built with Visual Studio 2022 17.10 or later constructs it, at compile time: 80 bytes -- the
// type (plain | try), 64 bytes for the lock (all zero), the owning thread (-1) and the count (0).
struct NewMutex {
    alignas(8) unsigned char raw[80];
    NewMutex() {
        memset(raw, 0, sizeof raw);
        const int type = 2;
        const long owner = -1;
        memcpy(raw, &type, 4);
        memcpy(raw + 72, &owner, 4);
    }
};

int Guarded(MtxFn fn, void* m) {   // 0 fine, 1 the function reported an error, 2 it faulted
    __try {
        return fn(m) == 0 ? 0 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: vrmod_crt_shim_test <path to the game's msvcp140.dll>\n"); return 2; }
    const HMODULE oldMod = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!oldMod) { printf("could not load %ls (error %lu)\n", argv[1], GetLastError()); return 2; }
    Check(GetModuleHandleW(L"msvcp140.dll") == oldMod, "the old msvcp140.dll is the process's msvcp140.dll, as in the game");
    const MtxFn lock = reinterpret_cast<MtxFn>(GetProcAddress(oldMod, "_Mtx_lock"));
    const MtxFn unlock = reinterpret_cast<MtxFn>(GetProcAddress(oldMod, "_Mtx_unlock"));
    const MtxInit init = reinterpret_cast<MtxInit>(GetProcAddress(oldMod, "_Mtx_init_in_situ"));
    const MtxVoid destroy = reinterpret_cast<MtxVoid>(GetProcAddress(oldMod, "_Mtx_destroy_in_situ"));
    if (!lock || !unlock || !init || !destroy) { printf("the old msvcp140.dll lacks the mutex functions\n"); return 2; }

    // Before: the crash itself.
    NewMutex fresh;
    Check(Guarded(lock, fresh.raw) == 2, "before the shim: locking a compile-time mutex in the old runtime FAULTS (the crash seen in the game)");

    // An old-style mutex, made and locked by the old runtime before the shim: unlocked after it.
    alignas(8) unsigned char oldStyle[80];
    init(oldStyle, 2);
    Check(Guarded(lock, oldStyle) == 0, "before the shim: the old runtime locks a mutex it initialised itself");

    MH_Initialize();
    vrmod::CrtMutexShim();

    Check(Guarded(unlock, oldStyle) == 0, "after the shim: the mutex the old runtime locked is unlocked through the newer one");
    Check(Guarded(lock, oldStyle) == 0 && Guarded(unlock, oldStyle) == 0, "after the shim: an old-style mutex locks and unlocks");
    NewMutex fresh2;
    Check(Guarded(lock, fresh2.raw) == 0, "after the shim: a compile-time mutex LOCKS (no fault)");
    Check(Guarded(unlock, fresh2.raw) == 0, "after the shim: ...and unlocks");
    // Many times over, both kinds, as a layer would in use.
    bool many = true;
    for (int i = 0; i < 100000 && many; ++i) many = Guarded(lock, fresh2.raw) == 0 && Guarded(unlock, fresh2.raw) == 0 && Guarded(lock, oldStyle) == 0
                                                    && Guarded(unlock, oldStyle) == 0;
    Check(many, "after the shim: 100000 lock / unlock pairs of each kind");
    // A mutex initialised after the shim (the game's own code goes on making them).
    alignas(8) unsigned char later[80];
    init(later, 2);
    Check(Guarded(lock, later) == 0 && Guarded(unlock, later) == 0, "after the shim: a mutex initialised now locks and unlocks");
    destroy(later);
    destroy(oldStyle);
    printf("%s (%d failed)\n", g_failed ? "FAILED" : "all passed", g_failed);
    return g_failed ? 1 : 0;
}
