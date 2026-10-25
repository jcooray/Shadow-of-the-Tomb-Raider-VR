#include "build_config.h"
#include "frame_log.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "audio_cue.h"
#include "camera_interceptor.h"
#include "logger.h"
#include "perf_stats.h"
#include "settings.h"

namespace vrmod {

namespace {

constexpr size_t kRows = 32768;      // ~7.5 min at 72 fps, ~22 min at 24 fps
constexpr size_t kCamRecs = 131072;  // two writes per frame, with room to spare
constexpr size_t kGpuRing = kRows;   // GPU results are looked up when the file is written, so match the row ring
const float kNaN = std::nanf("");

struct CamRec {
    uint64_t index = 0;   // 1-based count of camera writes since launch
    double tMs = 0.0;
    uint32_t tid = 0;
    int flip = 0;
    uint64_t gameFrame = 0;
    CamPose pre{}, game{}, ours{};
    float persistPosCm = kNaN, persistRotDeg = kNaN, persistFov = kNaN;   // pre vs the previous write's ours
};

struct Row {
    uint64_t frame = 0, xrFrame = 0;
    uint32_t tid = 0;
    char eye = 'S';
    int gameState = -1, menuOpen = -1, movie = -1;   // from the game's own state, polled during this Present
    double tIn = 0.0, tOut = 0.0;
    bool haveXr = false;
    double tWait = 0.0;
    int64_t pdtNs = 0;
    double pdtMs = 0.0, periodMs = 0.0;
    bool shouldRender = false;
    uint64_t camIndex = 0;   // latest camera write before this Present (0 = none)
    uint64_t otherWrites = 0;   // cumulative writes to other structs at Present entry
    uintptr_t structAddr = 0;
    bool haveNow = false;
    float nowPosCm = kNaN, nowRotDeg = kNaN, nowFov = kNaN;   // struct at Present entry vs that write's ours
    bool haveViews = false;
    float rtFov[2][4]{}, subFov[2][4]{}, reproj[2]{};
    double sampleAgeMs = 0.0;
    uint32_t present1 = 0, dbgMsgs = 0;
    uint64_t aerFrame = 0;
    double predErrMs = std::nan(""), xrWaitMs = std::nan("");
    char fallback = 0;
    int displayStep = 0;
    float eyeAge[2] = { kNaN, kNaN };
    float refreshHz = kNaN;
};

std::atomic<bool> g_recording{false};
std::atomic<bool> g_tint{false};
std::atomic<bool> g_flip{false};
std::atomic<int> g_flipForced{0};   // FlipMode; 0 = not forced
std::atomic<char> g_eyeTag{'S'};
std::atomic<uint32_t> g_present1{0};
std::atomic<uint32_t> g_dbgMsgs{0};
std::atomic<uint64_t> g_gpuTag[kGpuRing];
std::atomic<float> g_gpuMs[kGpuRing];

// Camera writes: the game thread appends while recording; the present thread reads.
std::atomic<CamRec*> g_camRing{nullptr};
std::atomic<uint64_t> g_camLatest{0};
uint64_t g_camFirstRecorded = 1;

// Present thread only.
std::unique_ptr<Row[]> g_rows;
uint64_t g_rowsWritten = 0;
Row g_cur;
bool g_inPresent = false;
uint64_t g_curFrame = 0;
bool g_autostartDone = false;
double g_recordStartMs = 0.0;

std::mutex g_metaMutex;
std::map<std::string, std::string> g_meta;

float PosErrCm(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float RotErrDeg(const float a[4], const float b[4]) {
    const float na = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
    const float nb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
    if (na <= 0.0f || nb <= 0.0f) return kNaN;
    float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) / (na * nb);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * std::acos(d) * 57.29578f;
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void Put(std::ostringstream& o, double v, int prec) {
    o << ',';
    if (!std::isnan(v)) {
        o.precision(prec);
        o << v;
    }
}

void PutPose(std::ostringstream& o, const CamPose& p) {
    for (float v : p.p) Put(o, v, 3);
    for (float v : p.q) Put(o, v, 6);
    Put(o, p.fov, 6);
}

// Same rule as the menu detection: gameplay = playing-state 5, no menu, no video (unknown counts as "not").
bool InGameplay(int state, int menu, int movie) {
    return state == 5 && menu != 1 && movie != 1;
}

// The game prints its active settings to its own log whenever they are applied, ending with "End of settings.".
// Returns the most recent block (the part after "[Settings] " on each line), or a note if none is found.
std::string ReadGameSettingsBlock() {
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || !docs) {
        return "(Documents folder not found)";
    }
    const std::wstring path = std::wstring(docs) + L"\\Shadow of the Tomb Raider\\Shadow of the Tomb Raider.log";
    CoTaskMemFree(docs);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return "(game log not found)";
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    const LONGLONG want = (std::min)(size.QuadPart, static_cast<LONGLONG>(512 * 1024));
    LARGE_INTEGER start{};
    start.QuadPart = size.QuadPart - want;
    SetFilePointerEx(h, start, nullptr, FILE_BEGIN);
    std::string text(static_cast<size_t>(want), '\0');
    DWORD got = 0;
    ReadFile(h, text.data(), static_cast<DWORD>(want), &got, nullptr);
    CloseHandle(h);
    text.resize(got);

    const size_t begin = text.rfind("[Settings] DirectX Version:");
    if (begin == std::string::npos) {
        return "(no settings block in the last 512 KB of the game log)";
    }
    std::string out;
    size_t pos = text.rfind('\n', begin);
    pos = (pos == std::string::npos) ? 0 : pos + 1;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = eol + 1;
        const size_t tag = line.find("[Settings] ");
        if (tag == std::string::npos) {
            if (line.empty()) continue;   // the game prints a blank line inside the block
            break;
        }
        const std::string item = line.substr(tag + 11);
        out += "\n  " + item;
        if (item.find("End of settings.") != std::string::npos) break;
    }
    return out;
}

const char* kPoseCols[] = { "px", "py", "pz", "qx", "qy", "qz", "qw", "fov" };

void PoseHeader(std::ostringstream& o, const char* prefix) {
    for (const char* c : kPoseCols) o << ',' << prefix << c;
}

struct Snapshot {
    std::vector<Row> rows;
    std::vector<CamRec> cams;   // contiguous, ascending index
    std::map<std::string, std::string> meta;
    bool wrapped = false;
};

const CamRec* FindCam(const Snapshot& s, uint64_t index) {
    auto it = std::lower_bound(s.cams.begin(), s.cams.end(), index,
        [](const CamRec& r, uint64_t v) { return r.index < v; });
    return (it != s.cams.end() && it->index == index) ? &*it : nullptr;
}

void WriteFiles(Snapshot s) {
    const std::string stamp = TimeStamp();
    const std::string framesName = "vrmod_frames_" + stamp + ".csv";
    const std::string camsName = "vrmod_camwrites_" + stamp + ".csv";
    const std::string metaName = "vrmod_frames_" + stamp + "_meta.txt";

    std::ostringstream f;
    f.setf(std::ios::fixed);
    f << "frame,xr_frame,present_tid,eye,game_state,menu_open,movie,in_gameplay,t_present_in_ms,t_present_out_ms,t_waitframe_ret_ms,predicted_display_ns,"
         "predicted_display_ms,display_period_ms,should_render,cam_struct_addr,other_struct_writes,"
         "cam_write_index,cam_writes_since_prev,cam_tid,t_cam_write_ms,flip,cam_game_frame";
    PoseHeader(f, "wr_");
    PoseHeader(f, "rb_");
    PoseHeader(f, "game_");
    f << ",persist_pos_cm,persist_rot_deg,persist_fov,atpresent_pos_cm,atpresent_rot_deg,atpresent_fov,gpu_ms";
    for (const char* e : { "L", "R" }) for (const char* s : { "left", "right", "up", "down" }) f << ",rt_" << e << '_' << s;
    for (const char* e : { "L", "R" }) for (const char* s : { "left", "right", "up", "down" }) f << ",sub_" << e << '_' << s;
    f << ",reproj_L_deg,reproj_R_deg,sample_age_ms,present1_calls,d3d12_debug_msgs,aer_game_frame,pred_err_ms,xr_wait_ms,fallback,display_step,eye_age_L,eye_age_R,refresh_hz\n";

    uint64_t prevCam = 0;
    const Row* prevRow = nullptr;
    for (const Row& r : s.rows) {
        f << r.frame << ',' << r.xrFrame << ',' << r.tid << ',' << r.eye << ','
          << r.gameState << ',' << r.menuOpen << ',' << r.movie << ',' << (InGameplay(r.gameState, r.menuOpen, r.movie) ? 1 : 0);
        Put(f, r.tIn, 3);
        Put(f, r.tOut, 3);
        if (r.haveXr) {
            Put(f, r.tWait, 3);
            f << ',' << r.pdtNs;
            Put(f, r.pdtMs, 3);
            Put(f, r.periodMs, 3);
            f << ',' << (r.shouldRender ? 1 : 0);
        } else {
            f << ",,,,,";
        }
        f << ",0x" << std::hex << r.structAddr << std::dec << ',';
        if (prevRow) f << (r.otherWrites - prevRow->otherWrites);
        prevRow = &r;
        const CamRec* c = r.camIndex ? FindCam(s, r.camIndex) : nullptr;
        const CamRec* next = c ? FindCam(s, r.camIndex + 1) : nullptr;   // "read back just before the next write"
        if (c) {
            f << ',' << c->index << ',';
            if (prevCam) f << (c->index - prevCam);   // blank on the first row: nothing to count from
            f << ',' << c->tid;
            Put(f, c->tMs, 3);
            f << ',' << c->flip << ',' << c->gameFrame;
            PutPose(f, c->ours);
            if (next) PutPose(f, next->pre); else f << ",,,,,,,,";
            PutPose(f, c->game);
            if (next) {
                Put(f, PosErrCm(next->pre.p, c->ours.p), 4);
                Put(f, RotErrDeg(next->pre.q, c->ours.q), 4);
                Put(f, std::fabs(next->pre.fov - c->ours.fov), 6);
            } else {
                f << ",,,";
            }
            prevCam = c->index;
        } else {
            f << ",,,,,,";   // cam_write_index .. cam_game_frame
            f << std::string(24 + 3, ',');
        }
        if (r.haveNow) {
            Put(f, r.nowPosCm, 4);
            Put(f, r.nowRotDeg, 4);
            Put(f, r.nowFov, 6);
        } else {
            f << ",,,";
        }
        const size_t gi = static_cast<size_t>(r.frame % kGpuRing);
        if (g_gpuTag[gi].load(std::memory_order_acquire) == r.frame) Put(f, g_gpuMs[gi].load(), 3); else f << ',';
        if (r.haveViews) {
            for (int e = 0; e < 2; ++e) for (int k = 0; k < 4; ++k) Put(f, r.rtFov[e][k], 5);
            for (int e = 0; e < 2; ++e) for (int k = 0; k < 4; ++k) Put(f, r.subFov[e][k], 5);
            Put(f, r.reproj[0], 3);
            Put(f, r.reproj[1], 3);
            Put(f, r.sampleAgeMs, 3);
        } else {
            f << std::string(19, ',');
        }
        f << ',' << r.present1 << ',' << r.dbgMsgs << ',';
        if (r.aerFrame) f << r.aerFrame;
        Put(f, r.predErrMs, 3);
        Put(f, r.xrWaitMs, 3);
        f << ',';
        if (r.fallback) f << r.fallback;
        f << ',' << r.displayStep;
        Put(f, r.eyeAge[0], 2);
        Put(f, r.eyeAge[1], 2);
        Put(f, r.refreshHz, 1);
        f << '\n';
    }

    std::ostringstream c;
    c.setf(std::ios::fixed);
    c << "cam_write_index,t_ms,tid,flip,game_frame";
    PoseHeader(c, "pre_");
    PoseHeader(c, "game_");
    PoseHeader(c, "ours_");
    c << ",persist_pos_cm,persist_rot_deg,persist_fov\n";
    for (const CamRec& r : s.cams) {
        c << r.index;
        Put(c, r.tMs, 3);
        c << ',' << r.tid << ',' << r.flip << ',' << r.gameFrame;
        PutPose(c, r.pre);
        PutPose(c, r.game);
        PutPose(c, r.ours);
        Put(c, r.persistPosCm, 4);
        Put(c, r.persistRotDeg, 4);
        Put(c, r.persistFov, 6);
        c << '\n';
    }

    std::ofstream(framesName, std::ios::binary) << f.str();
    std::ofstream(camsName, std::ios::binary) << c.str();
    {
        std::ofstream m(metaName, std::ios::binary);
        m << "rows=" << s.rows.size() << "\ncamera_writes=" << s.cams.size()
          << "\nring_wrapped=" << (s.wrapped ? 1 : 0) << "\n";
        for (const auto& kv : s.meta) m << kv.first << '=' << kv.second << '\n';
    }
    modlog::Line("FrameLog: wrote " + std::to_string(s.rows.size()) + " frames to " + framesName + " and "
        + std::to_string(s.cams.size()) + " camera writes to " + camsName
        + (s.wrapped ? " (recording was longer than the buffer; only the most recent part was kept)" : ""));
}

Snapshot TakeSnapshot() {
    Snapshot s;
    const uint64_t n = g_rowsWritten < kRows ? g_rowsWritten : kRows;
    s.wrapped = g_rowsWritten > kRows;
    s.rows.reserve(static_cast<size_t>(n));
    for (uint64_t i = g_rowsWritten - n; i < g_rowsWritten; ++i) s.rows.push_back(g_rows[static_cast<size_t>(i % kRows)]);

    CamRec* ring = g_camRing.load(std::memory_order_acquire);
    const uint64_t latest = g_camLatest.load(std::memory_order_acquire);
    if (ring && latest >= g_camFirstRecorded) {
        uint64_t first = g_camFirstRecorded;
        if (latest - first + 1 > kCamRecs - 16) {   // keep clear of the slot the game thread may be writing
            first = latest - (kCamRecs - 16) + 1;
            s.wrapped = true;
        }
        s.cams.reserve(static_cast<size_t>(latest - first + 1));
        for (uint64_t i = first; i <= latest; ++i) {
            const CamRec& r = ring[static_cast<size_t>(i % kCamRecs)];
            if (r.index == i) s.cams.push_back(r);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_metaMutex);
        s.meta = g_meta;
    }
    s.meta["recorded_seconds"] = std::to_string((NowMs() - g_recordStartMs) / 1000.0);
    s.meta["game_settings_at_stop"] = ReadGameSettingsBlock();
    return s;
}

void StartRecording() {
    if (!g_rows) g_rows.reset(new Row[kRows]);
    if (!g_camRing.load()) g_camRing.store(new CamRec[kCamRecs], std::memory_order_release);
    g_rowsWritten = 0;
    g_camFirstRecorded = g_camLatest.load() + 1;
    g_recordStartMs = NowMs();
    FrameLogSetMeta("game_settings_at_start", ReadGameSettingsBlock());
    g_recording.store(true);
    modlog::Line("FrameLog: recording ON (Ctrl+J again to stop and write the CSVs).");
    PlayCue(Cue::On);
}

void StopRecording(bool synchronous) {
    if (!g_recording.exchange(false)) return;
    g_inPresent = false;
    Snapshot s = TakeSnapshot();
    modlog::Line("FrameLog: recording OFF, writing " + std::to_string(s.rows.size()) + " frames...");
    if (synchronous) {
        WriteFiles(std::move(s));
    } else {
        std::thread(WriteFiles, std::move(s)).detach();
        PlayCue(Cue::Off);
    }
}

bool CtrlOnly() {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
}

bool Edge(int vk, bool& wasDown) {
    const bool d = (VRMOD_DEV_KEY(vk) & 0x8000) != 0;
    const bool pressed = d && !wasDown;
    wasDown = d;
    return pressed && CtrlOnly();
}

} // namespace

void FrameLogCameraWrite(const CamPose& pre, const CamPose& game, const CamPose& ours, int flipSign, uint64_t gameFrame) {
    static CamPose prevOurs{};
    static bool havePrev = false;
    const uint64_t index = g_camLatest.load(std::memory_order_relaxed) + 1;
    CamRec* ring = g_camRing.load(std::memory_order_acquire);
    if (ring && g_recording.load(std::memory_order_relaxed)) {
        CamRec& r = ring[static_cast<size_t>(index % kCamRecs)];
        r.index = index;
        r.tMs = NowMs();
        r.tid = GetCurrentThreadId();
        r.flip = flipSign;
        r.gameFrame = gameFrame;
        r.pre = pre;
        r.game = game;
        r.ours = ours;
        if (havePrev) {
            r.persistPosCm = PosErrCm(pre.p, prevOurs.p);
            r.persistRotDeg = RotErrDeg(pre.q, prevOurs.q);
            r.persistFov = std::fabs(pre.fov - prevOurs.fov);
        } else {
            r.persistPosCm = r.persistRotDeg = r.persistFov = kNaN;
        }
    }
    prevOurs = ours;
    havePrev = true;
    g_camLatest.store(index, std::memory_order_release);
}

void FrameLogPresentBegin(uint64_t presentFrame, uint64_t xrFrame, const CamPose* structAtPresent) {
    g_curFrame = presentFrame;
    // [debug] frame_csv=1: start at the first gameplay frame, not at the title screen (which also runs in-engine).
    if (!g_autostartDone && GetDebugSwitches().frameCsv) {
        int st, menu, movie;
        GameUiSnapshot(&st, &menu, &movie);
        if (InGameplay(st, menu, movie)) {
            g_autostartDone = true;
            modlog::Line("FrameLog: [debug] frame_csv=1, first gameplay frame reached -- recording.");
            StartRecording();
        }
    }
    const uint32_t p1 = g_present1.exchange(0);
    const uint32_t dbg = g_dbgMsgs.exchange(0);
    if (!g_recording.load()) {
        g_inPresent = false;
        return;
    }
    g_cur = Row{};
    g_cur.frame = presentFrame;
    g_cur.xrFrame = xrFrame;
    g_cur.tid = GetCurrentThreadId();
    g_cur.tIn = NowMs();
    g_cur.eye = g_eyeTag.load(std::memory_order_relaxed);
    g_cur.present1 = p1;
    g_cur.dbgMsgs = dbg;
    g_cur.otherWrites = CameraOtherStructWrites();
    g_cur.structAddr = CameraStructAddress();
    const uint64_t latest = g_camLatest.load(std::memory_order_acquire);
    CamRec* ring = g_camRing.load(std::memory_order_acquire);
    if (ring && latest >= g_camFirstRecorded) {
        g_cur.camIndex = latest;
        const CamRec& c = ring[static_cast<size_t>(latest % kCamRecs)];
        if (structAtPresent && c.index == latest) {
            g_cur.haveNow = true;
            g_cur.nowPosCm = PosErrCm(structAtPresent->p, c.ours.p);
            g_cur.nowRotDeg = RotErrDeg(structAtPresent->q, c.ours.q);
            g_cur.nowFov = std::fabs(structAtPresent->fov - c.ours.fov);
        }
    }
    g_inPresent = true;
}

void FrameLogXrFrame(double waitReturnMs, int64_t predictedDisplayTimeNs, double predictedDisplayMs,
                     double periodMs, bool shouldRender) {
    if (!g_inPresent) return;
    g_cur.haveXr = true;
    g_cur.tWait = waitReturnMs;
    g_cur.pdtNs = predictedDisplayTimeNs;
    g_cur.pdtMs = predictedDisplayMs;
    g_cur.periodMs = periodMs;
    g_cur.shouldRender = shouldRender;
}

void FrameLogViews(const float runtimeFov[2][4], const float submittedFov[2][4], const float reprojDeg[2],
                   double sampleAgeMs) {
    if (!g_inPresent) return;
    g_cur.haveViews = true;
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < 4; ++k) {
            g_cur.rtFov[e][k] = runtimeFov[e][k];
            g_cur.subFov[e][k] = submittedFov[e][k];
        }
        g_cur.reproj[e] = reprojDeg[e];
    }
    g_cur.sampleAgeMs = sampleAgeMs;
}

void FrameLogAer(uint64_t gameFrame, double predErrMs, double xrWaitMs) {
    if (!g_inPresent) return;
    g_cur.aerFrame = gameFrame;
    g_cur.predErrMs = predErrMs;
    g_cur.xrWaitMs = xrWaitMs;
}

void FrameLogPace(char fallback, int displayStep, float eyeAgeL, float eyeAgeR, float refreshHz) {
    if (!g_inPresent) return;
    g_cur.fallback = fallback;
    g_cur.displayStep = displayStep;
    g_cur.eyeAge[0] = eyeAgeL;
    g_cur.eyeAge[1] = eyeAgeR;
    g_cur.refreshHz = refreshHz > 0.0f ? refreshHz : kNaN;
}

void FrameLogPresentEnd() {
    if (!g_inPresent || !g_recording.load()) return;
    g_cur.tOut = NowMs();
    GameUiSnapshot(&g_cur.gameState, &g_cur.menuOpen, &g_cur.movie);   // polled earlier in this same Present
    g_rows[static_cast<size_t>(g_rowsWritten % kRows)] = g_cur;
    ++g_rowsWritten;
    g_inPresent = false;
}

uint64_t FrameLogCurrentFrame() {
    return g_curFrame;
}

void FrameLogNotePresent1() {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        modlog::Line("FrameLog: the game called IDXGISwapChain1::Present1 (first call). Present1 frames are NOT "
                     "submitted to the headset -- this needs handling.");
    }
    g_present1.fetch_add(1);
}

void FrameLogAddDebugMessages(uint32_t count) {
    g_dbgMsgs.fetch_add(count);
}

void FrameLogSetGpuMs(uint64_t presentFrame, float ms) {
    const size_t i = static_cast<size_t>(presentFrame % kGpuRing);
    g_gpuMs[i].store(ms);
    g_gpuTag[i].store(presentFrame, std::memory_order_release);
}

void FrameLogSetMeta(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_metaMutex);
    g_meta[key] = value;
}

void FrameLogPollHotkeys() {
    static bool jWas = false, uWas = false, yWas = false;
    if (Edge('J', jWas)) {
        if (g_recording.load()) StopRecording(false); else StartRecording();
    }
    if (Edge('U', uWas)) {
        const bool on = !g_tint.load();
        g_tint.store(on);
        modlog::Line(std::string("FrameLog: eye tint ") + (on ? "ON (red block = left eye image, green = right)" : "OFF"));
        PlayCue(on ? Cue::On : Cue::Off);
    }
    if (Edge('Y', yWas)) {
        const bool on = !g_flip.load();
        g_flip.store(on);
        modlog::Line(std::string("FrameLog: flip test ") + (on ? "ON (camera pitched +60/-60 deg on alternate frames)" : "OFF"));
        PlayCue(on ? Cue::On : Cue::Off);
    }
}

void FrameLogShutdown() {
    StopRecording(true);
}

bool FrameLogTintEnabled() {
    return g_tint.load(std::memory_order_relaxed);
}

FlipMode FrameLogFlipMode() {
    const int forced = g_flipForced.load(std::memory_order_relaxed);
    if (forced != 0) return static_cast<FlipMode>(forced);
    return g_flip.load(std::memory_order_relaxed) ? FlipMode::Alternate : FlipMode::Off;
}

void FrameLogForceFlipMode(FlipMode mode) {
    g_flipForced.store(static_cast<int>(mode), std::memory_order_relaxed);
}

namespace {
std::atomic<uint64_t> g_flipSeed{0x9E3779B97F4A7C15ull};
}

void FrameLogSetFlipSeed(uint64_t seed) {
    g_flipSeed.store(seed, std::memory_order_relaxed);
}

int FrameLogRandomFlipSign(uint64_t gameFrame) {
    // splitmix64 of (frame + seed): a well-mixed bit per frame, reproducible for the analysis.
    uint64_t z = gameFrame + g_flipSeed.load(std::memory_order_relaxed);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (z & 1) ? 1 : -1;
}

void FrameLogStartRecording() {
    if (!g_recording.load()) StartRecording();
}

void FrameLogStopRecording() {
    StopRecording(false);
}

bool FrameLogIsRecording() {
    return g_recording.load();
}

void FrameLogSetEyeTag(char tag) {
    g_eyeTag.store(tag, std::memory_order_relaxed);
    if (g_inPresent) g_cur.eye = tag;   // the row being built (called from the Present thread)
}

std::string FrameLogReadGameSettings() {
    return ReadGameSettingsBlock();
}

}
