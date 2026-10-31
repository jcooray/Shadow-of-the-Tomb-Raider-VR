#include "exit_hook.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "logger.h"
#include "openxr_integration.h"
#include "frame_log.h"

namespace vrmod {

namespace {

using ExitProcess_t = VOID(WINAPI*)(UINT);
using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);
ExitProcess_t oExitProcess = nullptr;
TerminateProcess_t oTerminateProcess = nullptr;
DWORD g_mainThread = 0;

// "module+0xoffset" for a code address, so a report can be looked up in that module.
std::string Where(DWORD64 addr) {
    HMODULE m = nullptr;
    char b[MAX_PATH + 40];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &m) || !m) {
        snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(addr));
        return b;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    snprintf(b, sizeof(b), "%ls+0x%llx", name ? name + 1 : path, static_cast<unsigned long long>(addr - reinterpret_cast<DWORD64>(m)));
    return b;
}

// The calling thread's stack, innermost first (the hook's own frame skipped).
std::string OwnStack() {
    void* frames[14] = {};
    const USHORT n = RtlCaptureStackBackTrace(1, 14, frames, nullptr);
    std::string s;
    for (USHORT i = 0; i < n; ++i) s += (i ? " < " : "") + Where(reinterpret_cast<DWORD64>(frames[i]));
    return s;
}

// One unwind step from a copied context; false when the stack can't be read (no C++ objects here: __try).
bool UnwindStep(CONTEXT* ctx) {
    __try {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx->Rip, &imageBase, nullptr);
        if (!fn) {   // a leaf function: the return address is on top of the stack
            ctx->Rip = *reinterpret_cast<DWORD64*>(ctx->Rsp);
            ctx->Rsp += 8;
        } else {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx->Rip, fn, ctx, &handlerData, &establisher, nullptr);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Another thread's stack: its registers are taken while it is suspended and the walk is done after it resumes (never
// call into the loader or the heap while a thread that may hold their locks is stopped). A thread that is stuck keeps
// its stack as it was, which is the case this is for.
std::string ThreadStack(DWORD tid) {
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!th) return "(the thread can't be opened)";
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    bool got = false;
    if (SuspendThread(th) != static_cast<DWORD>(-1)) {
        got = GetThreadContext(th, &ctx) != 0;
        ResumeThread(th);
    }
    CloseHandle(th);
    if (!got) return "(no registers)";
    std::string s;
    for (int i = 0; i < 16 && ctx.Rip; ++i) {
        s += (i ? " < " : "") + Where(ctx.Rip);
        if (!UnwindStep(&ctx)) break;
    }
    return s;
}

VOID WINAPI hkExitProcess(UINT exitCode) {
    modlog::Line("ExitProcess called (exit code " + std::to_string(exitCode) + ", from " + OwnStack() + "), tearing OpenXR down first.");
    FrameLogShutdown();   // writes the per-frame CSVs if a recording is running
    ShutdownOpenXR();
    oExitProcess(exitCode);
}

// The game ending itself without ExitProcess (a start that just vanished, 2026-10-31): who did it.
BOOL WINAPI hkTerminateProcess(HANDLE process, UINT exitCode) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) {
        modlog::Line("TerminateProcess on the game itself (exit code " + std::to_string(exitCode) + ", from " + OwnStack() + ")");
    }
    return oTerminateProcess(process, exitCode);
}

// A crash: an exception nothing handled. The game's crash reporter installs its own top-level filter; ours stays in
// front of it (SetUnhandledExceptionFilter hooked) and passes each crash on after logging it. A vectored handler would
// also see the exceptions the game and the mod handle themselves (several in a normal start).
using SetUef_t = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetUef_t oSetUnhandledExceptionFilter = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_gameFilter = nullptr;

LONG WINAPI CrashLogger(EXCEPTION_POINTERS* e) {
    static std::atomic<int> n{ 0 };
    if (n.fetch_add(1) < 2) {
        const DWORD code = e->ExceptionRecord->ExceptionCode;
        char b[160];
        snprintf(b, sizeof(b), "CRASH: exception 0x%08lX on thread %lu at ", code, GetCurrentThreadId());
        std::string line = b + Where(reinterpret_cast<DWORD64>(e->ExceptionRecord->ExceptionAddress));
        if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR kind = e->ExceptionRecord->ExceptionInformation[0];
            snprintf(b, sizeof(b), " (%s 0x%llx)", kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                     static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[1]));
            line += b;
        }
        if (code != EXCEPTION_STACK_OVERFLOW) {   // a stack overflow leaves too little stack to walk it
            CONTEXT ctx = *e->ContextRecord;
            line += "; stack";
            for (int i = 0; i < 16 && ctx.Rip; ++i) {
                line += (i ? " < " : " ") + Where(ctx.Rip);
                if (!UnwindStep(&ctx)) break;
            }
        }
        modlog::Line(line);
    }
    return g_gameFilter ? g_gameFilter(e) : EXCEPTION_CONTINUE_SEARCH;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hkSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = g_gameFilter;
    g_gameFilter = filter == CrashLogger ? nullptr : filter;
    return previous;
}

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM found) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    RECT r{};
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindowRect(hwnd, &r) && r.right - r.left >= 640) {
        *reinterpret_cast<bool*>(found) = true;
        return FALSE;
    }
    return TRUE;
}

// A start that hangs before the game's window appears (Quest 3 over Virtual Desktop, 2026-10-31): where the game's
// main thread is waiting, 30 s and 75 s in.
DWORD WINAPI StartWatch(LPVOID) {
    const DWORD marks[] = { 30000, 75000 };
    DWORD waited = 0;
    for (DWORD mark : marks) {
        Sleep(mark - waited);
        waited = mark;
        bool found = false;
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&found));
        if (found) return 0;
        modlog::Line("STARTUP: no game window after " + std::to_string(mark / 1000) + " s; the game's main thread (" + std::to_string(g_mainThread)
                     + ") is at " + ThreadStack(g_mainThread));
    }
    return 0;
}

} // namespace

void NoteMainThread() {
    g_mainThread = GetCurrentThreadId();
}

bool InstallExitHook() {
    // MH_Initialize() already called by InstallPresentHook() before this runs.
    void* target = nullptr;
    if (MH_CreateHookApiEx(L"kernel32", "ExitProcess", &hkExitProcess, reinterpret_cast<void**>(&oExitProcess), &target) != MH_OK
        || MH_EnableHook(target) != MH_OK) {
        modlog::Line("ExitHook: ExitProcess hook setup failed.");
        return false;
    }
    if (MH_CreateHookApiEx(L"kernelbase", "TerminateProcess", &hkTerminateProcess, reinterpret_cast<void**>(&oTerminateProcess), &target) != MH_OK
        || MH_EnableHook(target) != MH_OK) {
        modlog::Line("ExitHook: TerminateProcess hook setup failed.");
    }
    if (MH_CreateHookApiEx(L"kernelbase", "SetUnhandledExceptionFilter", &hkSetUnhandledExceptionFilter,
                           reinterpret_cast<void**>(&oSetUnhandledExceptionFilter), &target) == MH_OK && MH_EnableHook(target) == MH_OK) {
        g_gameFilter = oSetUnhandledExceptionFilter(CrashLogger);
    } else {
        modlog::Line("ExitHook: SetUnhandledExceptionFilter hook setup failed (crashes not logged).");
    }
    if (g_mainThread) CloseHandle(CreateThread(nullptr, 0, StartWatch, nullptr, 0, nullptr));
    modlog::Line("ExitHook: installed (exit, termination and crashes are logged with where they came from).");
    return true;
}

} // namespace vrmod
