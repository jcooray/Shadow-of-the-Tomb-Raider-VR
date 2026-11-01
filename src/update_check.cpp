#include "update_check.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "logger.h"
#include "settings.h"
#include "version.h"

namespace vrmod {

namespace {

// The latest release, as GitHub's REST API describes it (public, no sign-in; 60 requests an hour per address is far
// more than one per game start).
constexpr wchar_t kHost[] = L"api.github.com";
constexpr wchar_t kPath[] = L"/repos/jcooray/Shadow-of-the-Tomb-Raider-VR/releases/latest";
constexpr DWORD kDelayMs = 15000;   // after start-up, so the request never competes with the game loading

enum class State { NotStarted, Off, Checking, UpToDate, Newer, Failed };
std::atomic<State> g_state{State::NotStarted};
std::mutex g_mutex;
std::string g_version, g_notes;

// The body of one HTTPS GET as UTF-8; empty on any failure (logged).
std::string Get() {
    std::string out;
    const char* const ver = SOTTRVR_VERSION_STRING;   // one literal: the range below must be within it
    const std::wstring agent = L"SOTTR-VR/" + std::wstring(ver, ver + strlen(ver));
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return out;
    WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
    HINTERNET c = WinHttpConnect(s, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", kPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    DWORD status = 0, size = sizeof(status);
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(r, nullptr)
        && WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)
        && status == 200) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0 || out.size() > 512 * 1024) break;
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got) || got == 0) break;
            out.append(chunk.data(), got);
        }
    } else {
        modlog::Line("UPDATE: GitHub didn't answer (error " + std::to_string(GetLastError()) + ", status " + std::to_string(status) + ").");
        out.clear();
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return out;
}

// A JSON string value ("key": "..."), unescaped (\n \" \\ \uXXXX as UTF-8). Enough for GitHub's flat release fields.
bool JsonString(const std::string& json, const char* key, std::string* value) {
    const std::string k = std::string("\"") + key + "\"";
    size_t i = json.find(k);
    if (i == std::string::npos) return false;
    i = json.find(':', i + k.size());
    if (i == std::string::npos) return false;
    i = json.find('"', i);
    if (i == std::string::npos) return false;
    std::string v;
    for (++i; i < json.size() && json[i] != '"'; ++i) {
        char ch = json[i];
        if (ch != '\\' || i + 1 >= json.size()) { v += ch; continue; }
        ch = json[++i];
        switch (ch) {
        case 'n': v += '\n'; break;
        case 'r': break;
        case 't': v += ' '; break;
        case 'u': {
            if (i + 4 >= json.size()) return false;
            const unsigned cp = static_cast<unsigned>(strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
            if (cp < 0x80) v += static_cast<char>(cp);
            else if (cp < 0x800) { v += static_cast<char>(0xC0 | (cp >> 6)); v += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { v += static_cast<char>(0xE0 | (cp >> 12)); v += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); v += static_cast<char>(0x80 | (cp & 0x3F)); }
            break;
        }
        default: v += ch; break;
        }
    }
    *value = v;
    return true;
}

// "v1.2.0" / "1.2.0" -> comparable number; 0 if it doesn't parse.
unsigned long Version(const std::string& s) {
    unsigned a = 0, b = 0, c = 0;
    const char* p = s.c_str();
    if (*p == 'v' || *p == 'V') ++p;
    if (sscanf_s(p, "%u.%u.%u", &a, &b, &c) < 2) return 0;
    return a * 1000000ul + b * 1000ul + c;
}

// The release notes' "What's new" section as plain text for the overlay: bullets kept, Markdown links / bold / code
// marks dropped, at most about 700 characters (cut at a bullet).
std::string WhatsNew(const std::string& body) {
    size_t a = body.find("## What's new");
    a = a == std::string::npos ? 0 : body.find('\n', a);
    if (a == std::string::npos) return {};
    size_t b = body.find("\n## ", a + 1);
    const size_t w = body.find("\n**Watch", a + 1);
    if (w != std::string::npos && (b == std::string::npos || w < b)) b = w;
    const std::string sec = body.substr(a, b == std::string::npos ? std::string::npos : b - a);
    std::string out, line;
    const auto flush = [&] {
        if (line.empty()) return;
        if (!out.empty()) out += '\n';
        out += line;
        line.clear();
    };
    size_t i = 0;
    while (i <= sec.size()) {
        const size_t e = sec.find('\n', i);
        std::string l = sec.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? sec.size() + 1 : e + 1;
        size_t f = l.find_first_not_of(' ');
        if (f == std::string::npos) { flush(); continue; }
        const bool bullet = l.compare(f, 2, "- ") == 0;
        l = l.substr(f + (bullet ? 2 : 0));
        std::string t;   // [text](url) -> text; ** and ` dropped
        for (size_t k = 0; k < l.size(); ++k) {
            if (l.compare(k, 2, "**") == 0) { ++k; continue; }
            if (l[k] == '`') continue;
            if (l[k] == ']' && k + 1 < l.size() && l[k + 1] == '(') {
                const size_t close = l.find(')', k);
                if (close != std::string::npos) { k = close; continue; }
            }
            if (l[k] == '[') continue;
            t += l[k];
        }
        if (bullet) { flush(); line = "- " + t; }
        else line += (line.empty() ? "" : " ") + t;
    }
    flush();
    if (out.size() > 700) {
        size_t cut = out.rfind("\n- ", 700);
        out = out.substr(0, cut != std::string::npos && cut > 200 ? cut : 700) + "\n...";
    }
    return out;
}

void CheckBody();

// The thread: never lets an exception out (it would end the game).
void Check() {
    try {
        CheckBody();
    } catch (...) {
        g_state.store(State::Failed);
        modlog::Line("UPDATE: the check failed unexpectedly; no update notice this time.");
    }
}

void CheckBody() {
    Sleep(kDelayMs);
    const std::string json = Get();
    std::string tag, body;
    if (json.empty() || !JsonString(json, "tag_name", &tag)) {
        g_state.store(State::Failed);
        modlog::Line("UPDATE: couldn't read the latest release from GitHub.");
        return;
    }
    JsonString(json, "body", &body);
    const unsigned long latest = Version(tag), mine = Version(SOTTRVR_VERSION_STRING);
    if (!latest || latest <= mine) {
        g_state.store(State::UpToDate);
        modlog::Line("UPDATE: up to date (latest release " + tag + ", this is " + SOTTRVR_VERSION_STRING + ").");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
        g_notes = WhatsNew(body);
    }
    g_state.store(State::Newer);
    modlog::Line("UPDATE: " + tag + " is available (this is " + SOTTRVR_VERSION_STRING + ") -- the overlay says so.");
}

}  // namespace

void UpdateCheckStart() {
    if (g_state.load() != State::NotStarted) return;
    if (SettingGet("update.check") == 0.0f) {
        g_state.store(State::Off);
        modlog::Line("UPDATE: checking for updates is off ([update] check).");
        return;
    }
    g_state.store(State::Checking);
    std::thread(Check).detach();
}

bool UpdateAvailable(std::string* version, std::string* notes) {
    if (g_state.load() != State::Newer) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    *version = g_version;
    *notes = g_notes;
    return true;
}

std::string UpdateStatusText() {
    switch (g_state.load()) {
    case State::Off: return "off";
    case State::Checking: return "checking...";
    case State::UpToDate: return std::string("up to date (") + SOTTRVR_VERSION_STRING + ")";
    case State::Newer: {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_version + " available (this is " + SOTTRVR_VERSION_STRING + ")";
    }
    case State::Failed: return "couldn't reach GitHub";
    default: return "not checked yet";
    }
}

}  // namespace vrmod
