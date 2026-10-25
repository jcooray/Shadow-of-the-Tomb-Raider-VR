#include "steamvr_settings.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vrmod {

SteamVrScale ReadSteamVrScale() {
    SteamVrScale r;
    wchar_t steam[MAX_PATH] = {};
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) != ERROR_SUCCESS) return r;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(steam) + L"/config/steamvr.vrsettings").c_str(), L"rb") != 0 || !f) return r;
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    // The value after "key" : within [from, to) of the text; fallback if absent.
    auto value = [&s](const char* key, size_t from, size_t to, double fallback) {
        const size_t k = s.find(std::string("\"") + key + "\"", from);
        if (k == std::string::npos || k >= to) return fallback;
        const size_t c = s.find(':', k);
        if (c == std::string::npos || c >= to) return fallback;
        const char* p = s.c_str() + c + 1;
        while (*p == ' ' || *p == '\t') ++p;
        if (!strncmp(p, "true", 4)) return 1.0;
        if (!strncmp(p, "false", 5)) return 0.0;
        return atof(p);
    };
    auto block = [&s](const std::string& name, size_t* from, size_t* to) {
        const size_t k = s.find("\"" + name + "\"");
        if (k == std::string::npos) return false;
        *from = k;
        *to = s.find('}', k);
        return *to != std::string::npos;
    };
    size_t a = 0, b = 0;
    const bool haveSteamvr = block("steamvr", &a, &b);
    r.manual = haveSteamvr && value("supersampleManualOverride", a, b, 0.0) != 0.0;
    r.global = r.manual ? value("supersampleScale", a, b, 1.0) : value("gpuSpeedRenderTargetScale", 0, s.size(), 1.0);
    wchar_t id[32] = L"750920";   // SOTTR on Steam; SteamAppId is set when Steam starts the game
    GetEnvironmentVariableW(L"SteamAppId", id, 32);
    std::string app = "steam.app.";
    for (const wchar_t* c = id; *c; ++c) app += static_cast<char>(*c);
    if (block(app, &a, &b)) r.perApp = value("resolutionScale", a, b, 100.0);
    if (r.global <= 0.0) r.global = 1.0;
    if (r.perApp <= 0.0) r.perApp = 100.0;
    r.read = true;
    return r;
}

}
