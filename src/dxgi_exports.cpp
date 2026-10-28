// This DLL is dropped in the game's install folder as "dxgi.dll". Windows loads it
// automatically because the game imports dxgi.dll. Every export below just forwards
// to the real system DXGI so the game behaves exactly as normal, except we get to
// run our own init code first (see dllmain.cpp) and hook Present (see present_hook.cpp).
//
// We resolve the real dxgi.dll via its absolute System32 path rather than relying on
// the loader's search order -- if we used LoadLibraryW(L"dxgi.dll") here, the loader
// would find *this* DLL again (it's earlier in the search path than System32), causing
// infinite recursion.

#include <windows.h>
#include <unknwn.h>  // REFIID, without dxgi.h's conflicting dllimport declarations
#include <string>

#include "cheeky_loader.h"
#include "logger.h"
#include "resolution_unlock.h"

namespace {

HMODULE g_realDxgi = nullptr;

using PFN_CreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_DXGIGetDebugInterface1 = HRESULT(WINAPI*)(UINT, REFIID, void**);

PFN_CreateDXGIFactory g_pCreateDXGIFactory = nullptr;
PFN_CreateDXGIFactory1 g_pCreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 g_pCreateDXGIFactory2 = nullptr;
PFN_DXGIGetDebugInterface1 g_pDXGIGetDebugInterface1 = nullptr;

void EnsureRealDxgiLoaded() {
    if (g_realDxgi) {
        return;
    }

    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);

    std::wstring path = std::wstring(sysDir) + L"\\dxgi.dll";
    g_realDxgi = LoadLibraryW(path.c_str());
    if (!g_realDxgi) {
        modlog::Line("FATAL: could not load real system dxgi.dll, mod cannot proxy DXGI calls.");
        return;
    }

    g_pCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(
        GetProcAddress(g_realDxgi, "CreateDXGIFactory"));
    g_pCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(
        GetProcAddress(g_realDxgi, "CreateDXGIFactory1"));
    g_pCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
        GetProcAddress(g_realDxgi, "CreateDXGIFactory2"));
    g_pDXGIGetDebugInterface1 = reinterpret_cast<PFN_DXGIGetDebugInterface1>(
        GetProcAddress(g_realDxgi, "DXGIGetDebugInterface1"));

    modlog::Line("Real system dxgi.dll loaded and exports resolved.");
}

} // namespace

namespace vrmod {

HRESULT RealCreateDXGIFactory1(REFIID riid, void** ppFactory) {
    EnsureRealDxgiLoaded();
    return g_pCreateDXGIFactory1 ? g_pCreateDXGIFactory1(riid, ppFactory) : E_FAIL;
}

} // namespace vrmod

extern "C" {

// The resolution hooks must be live before the game's first display-mode
// enumeration, which follows its first CreateDXGIFactory* call by a fraction of
// a second -- far quicker than the init thread gets there. So the first call
// from any thread installs them synchronously here, before the factory is
// handed back. (Idempotent; re-entrant calls from our own throwaway device
// creation return immediately.)
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    EnsureRealDxgiLoaded();
    vrmod::InstallResolutionUnlock(false);
    const HRESULT hr = g_pCreateDXGIFactory ? g_pCreateDXGIFactory(riid, ppFactory) : E_FAIL;
    if (SUCCEEDED(hr)) vrmod::CheekyLoaderAfterFactory();   // Cheeky's hooks in before the first swap chain
    return hr;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    EnsureRealDxgiLoaded();
    vrmod::InstallResolutionUnlock(false);
    const HRESULT hr = g_pCreateDXGIFactory1 ? g_pCreateDXGIFactory1(riid, ppFactory) : E_FAIL;
    if (SUCCEEDED(hr)) vrmod::CheekyLoaderAfterFactory();   // Cheeky's hooks in before the first swap chain
    return hr;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    EnsureRealDxgiLoaded();
    vrmod::InstallResolutionUnlock(false);
    const HRESULT hr = g_pCreateDXGIFactory2 ? g_pCreateDXGIFactory2(Flags, riid, ppFactory) : E_FAIL;
    if (SUCCEEDED(hr)) vrmod::CheekyLoaderAfterFactory();   // Cheeky's hooks in before the first swap chain
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug) {
    EnsureRealDxgiLoaded();
    return g_pDXGIGetDebugInterface1 ? g_pDXGIGetDebugInterface1(Flags, riid, pDebug) : E_FAIL;
}

} // extern "C"
