// SSH Win Player - browse a remote server over SSH (Windows' bundled OpenSSH)
// and play its video / audio files with libmpv.  Local files work too.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <ctime>
#include <deque>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mpv_dyn.h"
#include "sshconn.h"
#include "resume_xml.h"

MpvApi g_mpv;

// ---------------------------------------------------------------------------
// Constants / IDs
// ---------------------------------------------------------------------------
static const wchar_t* kAppName = L"SSH Win Player";
static const wchar_t* kMainClass = L"SshWinPlayerMain";
static const wchar_t* kVideoClass = L"SshWinPlayerVideo";
static const char* kScheme = "sshwp";
static const UINT WM_MPV_WAKEUP = WM_APP + 1;
static const UINT WM_STREAM_READY = WM_APP + 2;
static const UINT WM_SSH_LOST = WM_APP + 3;  // lParam: std::string* with ssh's error (owned by receiver)
static const double kWheelSeek = 5.0;       // seconds per wheel notch
static const double kWheelSeekShift = 30.0; // with Shift held

enum {
    IDC_HOST_LBL = 100, IDC_HOST, IDC_PORT_LBL, IDC_PORT, IDC_PASS_LBL, IDC_PASS, IDC_KEY_LBL, IDC_KEY,
    IDC_KEYBROWSE, IDC_CONNECT, IDC_LOCAL,
    IDC_UP, IDC_HOME, IDC_REFRESH, IDC_PATH, IDC_ALLFILES, IDC_LIST,
    IDC_SEEK, IDC_TIME, IDC_STATUS, IDC_VIDEO,
    // player buttons (keep contiguous)
    IDC_BTN_FIRST,
    IDC_BTN_PREV = IDC_BTN_FIRST, IDC_BTN_BACK60, IDC_BTN_BACK10, IDC_BTN_FRAMEBACK, IDC_BTN_PLAY,
    IDC_BTN_STOP, IDC_BTN_FRAMESTEP, IDC_BTN_FWD10, IDC_BTN_FWD60, IDC_BTN_NEXT,
    IDC_BTN_SLOWER, IDC_BTN_SPEED, IDC_BTN_FASTER, IDC_BTN_SUB, IDC_BTN_AUDIO, IDC_BTN_LOOP,
    IDC_BTN_FULL, IDC_BTN_LIST, IDC_BTN_OPEN,
    IDC_BTN_LAST
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE g_inst;
static HWND g_main, g_video, g_list, g_path, g_seek, g_time, g_status, g_tooltip;
static HWND g_host, g_port, g_pass, g_key, g_connectBtn, g_localBtn, g_allFiles, g_upBtn, g_homeBtn, g_refreshBtn;
static HWND g_lblHost, g_lblPort, g_lblPass, g_lblKey, g_keyBrowse;
static HWND g_btn[IDC_BTN_LAST - IDC_BTN_FIRST];
static HFONT g_font, g_symFont, g_bigFont;
static double g_scale = 1.0;
static int g_listW = 340;
static bool g_showList = true;
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_wpPrev = {sizeof(WINDOWPLACEMENT)};
static bool g_splitDrag = false;
static std::wstring g_iniPath;

static mpv_handle* g_mpvh = nullptr;
static SshConn g_browseConn;  // directory listings (UI thread)
static SshConn g_streamConn;  // file data (mpv threads)
static bool g_connected = false;
static bool g_remoteIsWindows = false; // Windows server (uname MINGW/MSYS/CYGWIN, or %USERPROFILE%)
static bool g_remoteWinPaths = false;  // PowerShell server: paths are "/C:/dir" internally, shown as C:\dir
static std::string g_remoteHome;        // Home target: ~ (Unix) or %USERPROFILE% (Windows)
static std::wstring g_remoteOs;       // uname -s, e.g. "Linux"
static bool g_remoteIsUnix = false;    // uname exists and is not a Windows POSIX layer
static bool g_resumeEnabled = false;   // shared resume positions in ~ on the server (Unix only)
static const wchar_t* kHotkeyHelpUrl = L"https://mpv.io/manual/stable/#keyboard-control";
static std::wstring g_connLabel;

// Player state mirrored from mpv properties.
static double g_timePos = 0, g_duration = 0, g_speed = 1.0;
static bool g_paused = false, g_idle = true;
static std::wstring g_mediaTitle;
static bool g_seekDragging = false;
static DWORD g_lastDragSeek = 0;
static int g_wheelAccum = 0;

// Browser state.
enum class Mode { Local, Remote };
static Mode g_mode = Mode::Local;
static std::string g_remotePath;   // UTF-8 POSIX path
static std::wstring g_localPath;   // "" = drive list
struct Entry {
    std::wstring name;
    std::string raw;  // remote name (UTF-8), or UTF-8 of local name
    bool isDir = false;
    bool isUp = false;
    bool isDrive = false;
    int64_t size = -1;
};
static std::vector<Entry> g_entries;  // full listing
static std::vector<int> g_shown;      // indices into g_entries shown in the list

// Playlist.
struct PlayItem {
    std::string url;
    std::wstring name;
    std::wstring dir;  // folder it came from (for highlighting)
    bool remote = false;
    std::string remotePath;  // server path (remote items), key for resume positions
    bool archive = false;    // zip / rar / 7z: mpv expands it into the media files inside
    std::vector<std::string> subs;
};
static std::vector<PlayItem> g_playlist;
static int g_plIndex = -1;
static int64_t g_mpvPlPos = -1, g_mpvPlCount = 0;  // mpv's own playlist (entries of an opened archive)
static bool g_archiveSession = false;   // the current item is an archive
static bool g_archiveFiltered = false;  // its non-media entries were removed from mpv's playlist
static bool g_archiveFromStart = false; // Shift: ignore saved positions inside this archive
static int g_remotePlayIndex = -1;  // remote item last started; kept through connection errors (for Retry)
static std::vector<std::string> g_pendingSubs;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static std::mutex g_logMtx;
static FILE* g_logFile = nullptr;
void AppLog(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (!g_logFile) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            line.c_str());
    fflush(g_logFile);
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------
static int S(int v) { return (int)std::lround(v * g_scale); }

static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return s;
}

static std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

static std::wstring LowerExt(const std::wstring& name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";
    std::wstring e = name.substr(dot + 1);
    CharLowerBuffW(&e[0], (DWORD)e.size());
    return e;
}

static bool InList(const std::wstring& e, const wchar_t* const* list) {
    for (; *list; ++list) if (e == *list) return true;
    return false;
}
static const wchar_t* const kMediaExt[] = {
    // video
    L"mkv", L"mp4", L"m4v", L"avi", L"mov", L"wmv", L"flv", L"webm", L"mpg", L"mpeg", L"mpe", L"m2v",
    L"ts", L"m2ts", L"mts", L"vob", L"ogv", L"3gp", L"3g2", L"rm", L"rmvb", L"divx", L"asf", L"f4v",
    L"y4m", L"hevc", L"h264", L"264", L"265", L"av1", L"ivf", L"mxf", L"dv", L"nut",
    // audio
    L"mp3", L"flac", L"wav", L"ogg", L"oga", L"opus", L"m4a", L"m4b", L"aac", L"wma", L"ape", L"alac",
    L"aiff", L"aif", L"aifc", L"wv", L"mka", L"dsf", L"dff", L"mpc", L"tta", L"ac3", L"eac3", L"dts",
    L"thd", L"mp2", L"mpa", L"amr", L"au", L"caf", L"spx", L"mod", L"xm", L"it", L"s3m", L"mid", L"midi",
    nullptr};
static const wchar_t* const kSubExt[] = {L"srt", L"ass", L"ssa", L"vtt", L"sub", L"sup", L"smi", L"lrc", nullptr};

static bool IsMedia(const std::wstring& name) { return InList(LowerExt(name), kMediaExt); }
static bool IsSub(const std::wstring& name) { return InList(LowerExt(name), kSubExt); }

// Archives: libmpv-2.dll is built with libarchive, so mpv opens these itself
// and plays the media files inside (ZIP, RAR incl. RAR5 and multi-part sets,
// 7-Zip).
static const wchar_t* const kArchiveExt[] = {L"zip", L"rar", L"7z", nullptr};

// "Movie.part2.rar", "Movie.part03.rar": later parts of a multi-part RAR set.
// Only the first part is opened (mpv finds the others next to it).
static bool IsFollowUpVolume(const std::wstring& name) {
    std::wstring n = name;
    CharLowerBuffW(&n[0], (DWORD)n.size());
    if (n.size() < 10 || n.compare(n.size() - 4, 4, L".rar") != 0) return false;
    size_t part = n.rfind(L".part", n.size() - 4);
    if (part == std::wstring::npos) return false;
    std::wstring digits = n.substr(part + 5, n.size() - 4 - (part + 5));
    if (digits.empty() || digits.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
    return _wtoi(digits.c_str()) != 1;
}

static bool IsArchive(const std::wstring& name) { return InList(LowerExt(name), kArchiveExt) && !IsFollowUpVolume(name); }
// Shown in the "media only" list and used in the folder playlist.
static bool IsPlayable(const std::wstring& name) { return IsMedia(name) || IsArchive(name); }

static std::string PercentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
        else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
    }
    return r;
}

static std::string PercentDecode(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            r += s[i];
        }
    }
    return r;
}

static std::string RemoteUrl(const std::string& path) { return std::string(kScheme) + "://" + PercentEncode(path); }

static std::string JoinRemote(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

static std::string RemoteParent(const std::string& p) {
    if (p.empty() || p == "/") return "/";
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t i = s.rfind('/');
    if (i == std::string::npos || i == 0) return "/";
    return s.substr(0, i);
}

static std::wstring JoinLocal(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

static std::wstring LocalParent(const std::wstring& p) {
    std::wstring s = p;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    if (s.size() <= 3) return L"";  // drive root -> drive list
    size_t i = s.find_last_of(L"\\/");
    if (i == std::wstring::npos) return L"";
    if (i == 2) return s.substr(0, 3);  // "C:\"
    return s.substr(0, i);
}

static std::wstring FormatTime(double t) {
    if (t < 0 || std::isnan(t)) t = 0;
    long long s = (long long)t;
    wchar_t b[32];
    if (s >= 3600) swprintf(b, 32, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 32, L"%lld:%02lld", s / 60, s % 60);
    return b;
}

static std::wstring FormatSize(int64_t sz) {
    if (sz < 0) return L"";
    wchar_t b[32];
    const wchar_t* u[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)sz;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    if (i == 0) swprintf(b, 32, L"%lld B", (long long)sz);
    else swprintf(b, 32, L"%.1f %ls", v, u[i]);
    return b;
}

static void SetStatus(const std::wstring& s) { SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)s.c_str()); }
static void SetConnStatus() {
    std::wstring s = g_connected ? (L"Connected: " + g_connLabel + (g_remoteOs.empty() ? L"" : L"  (" + g_remoteOs + L")") +
                                    (g_resumeEnabled ? L"  \u2022 resume synced" : L""))
                                 : L"Not connected";
    SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)s.c_str());
}

// F1: open the mpv keyboard reference in the default browser.
static void OpenHotkeyHelp() {
    ShellExecuteW(nullptr, L"open", kHotkeyHelpUrl, nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Settings (INI in %APPDATA%\SshWinPlayer)
// ---------------------------------------------------------------------------
static std::wstring IniGet(const wchar_t* sec, const wchar_t* key, const wchar_t* def = L"") {
    wchar_t b[4096];
    GetPrivateProfileStringW(sec, key, def, b, 4096, g_iniPath.c_str());
    return b;
}
static void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.c_str(), g_iniPath.c_str());
}
static int IniGetInt(const wchar_t* sec, const wchar_t* key, int def) {
    return GetPrivateProfileIntW(sec, key, def, g_iniPath.c_str());
}

static std::wstring AppDataDir() {
    wchar_t b[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, b))) {
        std::wstring d = std::wstring(b) + L"\\SshWinPlayer";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }
    return L".";
}

// ---------------------------------------------------------------------------
// mpv helpers
// ---------------------------------------------------------------------------
static std::map<uint64_t, std::string> g_pendingCmds;  // async id -> command (for error logs)
static uint64_t g_cmdId = 1;

static void Cmd(std::initializer_list<std::string> args) {
    if (!g_mpvh) return;
    std::vector<const char*> v;
    std::string desc;
    for (auto& a : args) { v.push_back(a.c_str()); desc += a + " "; }
    v.push_back(nullptr);
    uint64_t id = g_cmdId++;
    g_pendingCmds[id] = desc;
    if (g_mpv.command_async(g_mpvh, id, v.data()) < 0) g_pendingCmds.erase(id);
}

static void SetPropFlag(const char* name, bool v) {
    if (!g_mpvh) return;
    int f = v ? 1 : 0;
    g_mpv.set_property_async(g_mpvh, 0, name, MPV_FORMAT_FLAG, &f);
}
static void SetPropDouble(const char* name, double v) {
    if (!g_mpvh) return;
    g_mpv.set_property_async(g_mpvh, 0, name, MPV_FORMAT_DOUBLE, &v);
}

static void SendKey(const std::string& name) {
    Cmd({"keypress", name});
}

static void SeekRel(double secs) {
    if (g_idle) return;
    char b[32];
    snprintf(b, sizeof(b), "%g", secs);
    Cmd({"seek", b, "relative"});
}

// ---------------------------------------------------------------------------
// Remote stream callbacks for mpv ("sshwp://<percent encoded path>")
//
// Each open remote file gets a background prefetch thread and a block cache.
// The thread keeps the data around the read position cached - 5 seconds
// ahead and 5 seconds behind (estimated from file size / duration) - and
// fetches it with pipelined requests, so a distant server is not limited to
// one block per round trip.  mpv reads are served from the cache.
// ---------------------------------------------------------------------------
static const int64_t kPage = 256 * 1024;       // cache block (multiple of SshConn::kBlock)
static const double kBufferSecs = 5.0;         // seconds to keep before and after the position
static const int64_t kMinWindow = 2 * 1024 * 1024;
static const int64_t kMaxWindow = 96 * 1024 * 1024;
static std::atomic<double> g_streamDuration{0};  // duration of the playing file (for byte rate)
// Measured keyframe (I-frame) interval of the playing file in seconds, 0 = unknown.
// The back buffers are sized from it so the keyframe needed for a precise seek
// a few seconds back is still in memory.
static std::atomic<double> g_keyframeInterval{0};
// Assumed until a longer interval is measured (x264's default maximum is 250
// frames = 10 s at 25 fps).  The buffers must be big enough from the start:
// once data has been dropped from the back buffer it is gone.
static const double kAssumedKeyframeInterval = 10.0;
static double KeyframeIntervalForBuffers() { return std::max(kAssumedKeyframeInterval, g_keyframeInterval.load()); }

struct RemoteStream {
    std::string path;
    int64_t size = 0;
    int64_t pos = 0;      // mpv's position (only touched by mpv's thread)
    std::mutex m;
    std::condition_variable cv;
    std::map<int64_t, std::vector<char>> pages;  // page index -> data
    int64_t cachedBytes = 0;
    std::set<int64_t> inflight;
    int64_t readPos = 0;  // last position mpv read/seeked (shared with worker)
    bool stop = false, failed = false;
    double linkRate = 0;  // measured bytes/s
    std::thread worker;
    // stats for the log
    int64_t fetched = 0;
    int waits = 0;
    double waitMs = 0;
};

static void RsWindows(const RemoteStream* rs, int64_t& ahead, int64_t& behind) {
    double d = g_streamDuration.load();
    double rate = d > 1.0 ? rs->size / d : 0.0;  // bytes per second of media
    int64_t five = (int64_t)(rate * kBufferSecs);
    // Behind: 5 s, or the keyframe interval when that is longer.
    int64_t back = (int64_t)(rate * std::max(kBufferSecs, KeyframeIntervalForBuffers()));
    ahead = std::max(kMinWindow, std::min(kMaxWindow, five));
    behind = std::max(kMinWindow / 2, std::min(kMaxWindow, back));
}

// Next page to fetch (caller holds rs->m), or -1 when the window is complete.
// Order: the page being read, a little ahead, the 5 s behind, then the rest
// of the 5 s ahead.
static int64_t RsNextWanted(RemoteStream* rs) {
    if (rs->stop || rs->failed || rs->size <= 0) return -1;
    int64_t last = (rs->size - 1) / kPage;
    int64_t cur = std::min(rs->readPos, rs->size - 1) / kPage;
    int64_t ahead, behind;
    RsWindows(rs, ahead, behind);
    int64_t aheadPages = (ahead + kPage - 1) / kPage;
    int64_t behindPages = (behind + kPage - 1) / kPage;
    auto want = [&](int64_t p) { return p >= 0 && p <= last && !rs->pages.count(p) && !rs->inflight.count(p); };
    int64_t first = std::min<int64_t>(aheadPages, 4);
    for (int64_t p = cur; p <= cur + first; ++p) if (want(p)) return p;
    for (int64_t p = cur - 1; p >= cur - behindPages; --p) if (want(p)) return p;
    for (int64_t p = cur + first + 1; p <= cur + aheadPages; ++p) if (want(p)) return p;
    return -1;
}

// Drop the pages farthest from the read position when over budget.
static void RsEvict(RemoteStream* rs) {
    int64_t ahead, behind;
    RsWindows(rs, ahead, behind);
    int64_t cap = ahead + behind + 16 * 1024 * 1024;
    int64_t cur = rs->readPos / kPage;
    while (rs->cachedBytes > cap && rs->pages.size() > 1) {
        auto lo = rs->pages.begin();
        auto hi = std::prev(rs->pages.end());
        auto victim = (cur - lo->first >= hi->first - cur) ? lo : hi;
        if (victim->first == cur) break;
        rs->cachedBytes -= (int64_t)victim->second.size();
        rs->pages.erase(victim);
    }
}

static void RsWorker(RemoteStream* rs) {
    int failures = 0;
    for (;;) {
        int depth, perSession;
        {
            std::unique_lock<std::mutex> lk(rs->m);
            rs->cv.wait(lk, [&] { return rs->stop || rs->failed || RsNextWanted(rs) >= 0; });
            if (rs->stop || rs->failed) break;
            // Keep about half a second of data in flight (8..32 blocks).
            double target = std::max(2.0 * 1024 * 1024, std::min(8.0 * 1024 * 1024, rs->linkRate * 0.5));
            depth = std::max(8, std::min(32, (int)(target / kPage)));
            // Hand the connection back after ~1 s of transfer so subtitle
            // streams / the next file can open without waiting long.
            perSession = std::max(depth, std::min(64, (int)(rs->linkRate / kPage)));
        }
        auto tPrev = std::chrono::steady_clock::now();
        bool firstAnswer = true;
        int r = g_streamConn.readPipelined(
            rs->path, kPage, depth, perSession,
            [&]() -> int64_t {
                std::lock_guard<std::mutex> lk(rs->m);
                int64_t p = RsNextWanted(rs);
                if (p < 0) return -1;
                rs->inflight.insert(p);
                return p * kPage;
            },
            [&](int64_t off, const char* data, int64_t n) {
                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - tPrev).count();
                tPrev = now;
                std::lock_guard<std::mutex> lk(rs->m);
                int64_t p = off / kPage;
                rs->inflight.erase(p);
                if (n >= 0) {
                    auto& page = rs->pages[p];
                    rs->cachedBytes -= (int64_t)page.size();
                    page.assign(data, data + n);
                    rs->cachedBytes += n;
                    rs->fetched += n;
                    // Link speed from the gaps between answers of a full pipeline.
                    if (!firstAnswer && dt > 0 && n == kPage) {
                        double rate = n / dt;
                        rs->linkRate = rs->linkRate > 0 ? rs->linkRate * 0.8 + rate * 0.2 : rate;
                    }
                    firstAnswer = false;
                    RsEvict(rs);
                }
                rs->cv.notify_all();
            });
        {
            std::lock_guard<std::mutex> lk(rs->m);
            rs->inflight.clear();
            if (r == 0 || (r < 0 && (++failures >= 3 || !g_streamConn.isEnabled()))) {
                rs->failed = true;
                rs->cv.notify_all();
                AppLog("stream failed: " + rs->path);
                break;
            }
            if (r > 0) failures = 0;
        }
        if (r < 0) Sleep(500);
        else std::this_thread::yield();  // let other streams use the connection
    }
}

static int64_t RsRead(void* cookie, char* out, uint64_t nbytes) {
    auto* rs = static_cast<RemoteStream*>(cookie);
    if (rs->pos >= rs->size) return 0;
    std::unique_lock<std::mutex> lk(rs->m);
    if (rs->readPos != rs->pos) {
        rs->readPos = rs->pos;
        rs->cv.notify_all();
    }
    int64_t p = rs->pos / kPage;
    auto it = rs->pages.find(p);
    if (it == rs->pages.end()) {
        auto t0 = std::chrono::steady_clock::now();
        rs->waits++;
        rs->cv.wait(lk, [&] { return rs->stop || rs->failed || rs->pages.count(p); });
        rs->waitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        it = rs->pages.find(p);
        if (it == rs->pages.end()) return -1;
    }
    int64_t inPage = rs->pos - p * kPage;
    int64_t avail = (int64_t)it->second.size() - inPage;
    if (avail <= 0) return 0;  // file shorter than expected
    int64_t k = std::min<int64_t>(avail, (int64_t)nbytes);
    memcpy(out, it->second.data() + inPage, (size_t)k);
    rs->pos += k;
    rs->readPos = rs->pos;
    if ((rs->pos / kPage) != p) rs->cv.notify_all();  // moved to a new page: prefetch further
    return k;
}

static int64_t RsSeek(void* cookie, int64_t offset) {
    auto* rs = static_cast<RemoteStream*>(cookie);
    if (offset < 0 || offset > rs->size) return MPV_ERROR_GENERIC;
    if (offset != rs->pos) AppLog("stream seek to byte " + std::to_string(offset));
    rs->pos = offset;
    std::lock_guard<std::mutex> lk(rs->m);
    rs->readPos = offset;
    rs->cv.notify_all();  // start fetching around the new position right away
    return offset;
}

static int64_t RsSize(void* cookie) { return static_cast<RemoteStream*>(cookie)->size; }

static void RsClose(void* cookie) {
    auto* rs = static_cast<RemoteStream*>(cookie);
    {
        std::lock_guard<std::mutex> lk(rs->m);
        rs->stop = true;
        rs->cv.notify_all();
    }
    if (rs->worker.joinable()) rs->worker.join();
    char b[256];
    snprintf(b, sizeof(b), "stream closed: fetched %.1f MiB, link %.1f MiB/s, %d waits (%.0f ms) - ",
             rs->fetched / 1048576.0, rs->linkRate / 1048576.0, rs->waits, rs->waitMs);
    AppLog(b + rs->path);
    delete rs;
}

static int RsOpen(void*, char* uri, mpv_stream_cb_info* info) {
    std::string u = uri;
    std::string prefix = std::string(kScheme) + "://";
    if (u.compare(0, prefix.size(), prefix) != 0) return MPV_ERROR_LOADING_FAILED;
    std::string path = PercentDecode(u.substr(prefix.size()));
    int64_t sz = 0;
    std::string err;
    if (!g_streamConn.size(path, sz, err)) {
        AppLog("open failed: " + path + ": " + err);
        return MPV_ERROR_LOADING_FAILED;
    }
    auto* rs = new RemoteStream;
    rs->path = path;
    rs->size = sz;
    rs->worker = std::thread(RsWorker, rs);  // starts buffering immediately
    info->cookie = rs;
    info->read_fn = RsRead;
    info->seek_fn = RsSeek;
    info->size_fn = RsSize;
    info->close_fn = RsClose;
    info->cancel_fn = nullptr;
    return 0;
}

// ---------------------------------------------------------------------------
// mpv setup
// ---------------------------------------------------------------------------
static const char* kBindings =
    // Mouse wheel seeks instead of changing the volume.
    "WHEEL_UP seek 5\n"
    "WHEEL_DOWN seek -5\n"
    "Shift+WHEEL_UP seek 30\n"
    "Shift+WHEEL_DOWN seek -30\n"
    "WHEEL_RIGHT seek 5\n"
    "WHEEL_LEFT seek -5\n"
    "Ctrl+WHEEL_UP ignore\n"
    "Ctrl+WHEEL_DOWN ignore\n"
    // Volume is fixed at 100% (use the system volume instead).
    "9 ignore\n"
    "0 ignore\n"
    "/ ignore\n"
    "* ignore\n"
    "m ignore\n"
    "KP_DIVIDE ignore\n"
    "KP_MULTIPLY ignore\n"
    "VOLUME_UP ignore\n"
    "VOLUME_DOWN ignore\n"
    "MUTE ignore\n"
    // Quitting the embedded player would kill it: map to stop instead.
    "q script-message swp-stop\n"
    "Q script-message swp-stop\n"
    "Ctrl+w script-message swp-stop\n"
    "CLOSE_WIN script-message swp-stop\n"
    "POWER ignore\n"
    "STOP script-message swp-stop\n"
    // F1: mpv keyboard reference in the default browser.
    "F1 script-message swp-help\n"
    // Playlist keys use the app's playlist (the files of the folder).
    "> script-message swp-next\n"
    "< script-message swp-prev\n"
    "ENTER script-message swp-next\n"
    "NEXT script-message swp-next\n"
    "PREV script-message swp-prev\n";

static void UpdateUiFromState();
static bool g_buffering = false;


// Tracks of the current file (from mpv's "track-list"), for the Subs / Audio menus.
struct Track {
    int64_t id = 0;
    std::string type;  // "video", "audio", "sub"
    std::wstring title, lang, codec;
    bool selected = false, external = false, isDefault = false;
};
static std::vector<Track> g_tracks;
static const int kMaxTrackMenu = 20;

static const mpv_node* NodeGet(const mpv_node* map, const char* key) {
    if (!map || map->format != MPV_FORMAT_NODE_MAP) return nullptr;
    for (int i = 0; i < map->u.list->num; ++i)
        if (strcmp(map->u.list->keys[i], key) == 0) return &map->u.list->values[i];
    return nullptr;
}
static std::wstring NodeStr(const mpv_node* m, const char* key) {
    const mpv_node* n = NodeGet(m, key);
    return n && n->format == MPV_FORMAT_STRING ? Utf8ToWide(n->u.string) : L"";
}
static bool NodeFlag(const mpv_node* m, const char* key) {
    const mpv_node* n = NodeGet(m, key);
    return n && n->format == MPV_FORMAT_FLAG && n->u.flag;
}

static void ParseTrackList(const mpv_node* arr) {
    g_tracks.clear();
    if (!arr || arr->format != MPV_FORMAT_NODE_ARRAY) return;
    for (int i = 0; i < arr->u.list->num; ++i) {
        const mpv_node* t = &arr->u.list->values[i];
        const mpv_node* id = NodeGet(t, "id");
        const mpv_node* type = NodeGet(t, "type");
        if (!id || id->format != MPV_FORMAT_INT64 || !type || type->format != MPV_FORMAT_STRING) continue;
        Track tr;
        tr.id = id->u.int64;
        tr.type = type->u.string;
        tr.title = NodeStr(t, "title");
        tr.lang = NodeStr(t, "lang");
        tr.codec = NodeStr(t, "codec");
        tr.selected = NodeFlag(t, "selected");
        tr.external = NodeFlag(t, "external");
        tr.isDefault = NodeFlag(t, "default");
        g_tracks.push_back(std::move(tr));
    }
}

static std::wstring TrackLabel(const Track& t) {
    std::wstring s = std::to_wstring(t.id) + L":  ";
    std::wstring title = t.title;
    if (title.empty()) title = t.lang.empty() ? L"Track " + std::to_wstring(t.id) : L"";
    s += title;
    if (!t.lang.empty()) s += (title.empty() ? L"" : L"  ") + std::wstring(L"[") + t.lang + L"]";
    std::wstring extra;
    if (!t.codec.empty()) extra += t.codec;
    if (t.external) extra += extra.empty() ? L"external" : L", external";
    if (t.isDefault) extra += extra.empty() ? L"default" : L", default";
    if (!extra.empty()) s += L"  (" + extra + L")";
    for (size_t i = 0; i < s.size(); ++i)  // '&' would become a menu accelerator
        if (s[i] == L'&') { s.insert(i, 1, L'&'); ++i; }
    return s;
}

// Drop-down list of the first 20 subtitle or audio tracks under the button.
// Shown when the button is held down (or right-clicked).  If the mouse
// button is still held, releasing it over an item selects that item.
static void ShowTrackMenu(HWND button, bool subs) {
    const char* type = subs ? "sub" : "audio";
    HMENU menu = CreatePopupMenu();
    const UINT kOff = 1, kBase = 100;
    bool anySelected = false;
    int total = 0, shown = 0;
    std::vector<int64_t> ids;
    const Track* beyond = nullptr;
    for (const Track& t : g_tracks) {
        if (t.type != type) continue;
        ++total;
        if (t.selected) anySelected = true;
        if (shown >= kMaxTrackMenu) {
            if (t.selected) beyond = &t;  // keep the current track visible
            continue;
        }
        AppendMenuW(menu, MF_STRING | (t.selected ? MF_CHECKED : 0), kBase + shown, TrackLabel(t).c_str());
        ids.push_back(t.id);
        ++shown;
    }
    if (subs) {
        InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING | (!anySelected ? MF_CHECKED : 0), kOff, L"Off");
        if (total) InsertMenuW(menu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }
    if (!total)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, subs ? L"No subtitles" : L"No audio tracks");
    if (total > shown) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (beyond) {
            AppendMenuW(menu, MF_STRING | MF_CHECKED, kBase + ids.size(), TrackLabel(*beyond).c_str());
            ids.push_back(beyond->id);
        }
        std::wstring more = L"... " + std::to_wstring(total - shown) + (subs ? L" more  ( j to cycle )" : L" more  ( # to cycle )");
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, more.c_str());
    }
    RECT r;
    GetWindowRect(button, &r);
    // Open upwards: the button sits at the bottom of the window.
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                              r.left, r.top, 0, g_main, nullptr);
    DestroyMenu(menu);
    const char* prop = subs ? "sid" : "aid";
    if (cmd == kOff) Cmd({"set", prop, "no"});
    else if (cmd >= kBase && cmd < kBase + ids.size()) Cmd({"set", prop, std::to_string(ids[cmd - kBase])});
    SetFocus(g_main);
}

// Subclass for the Subs / Audio buttons: click = cycle, hold = drop-down list.
static const UINT_PTR kHoldTimer = 77;
static const UINT kHoldMs = 400;
static WNDPROC g_trackBtnOrigProc;
static bool g_holdFired = false;

static LRESULT CALLBACK TrackBtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    bool subs = GetDlgCtrlID(h) == IDC_BTN_SUB;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        g_holdFired = false;
        SetTimer(h, kHoldTimer, kHoldMs, nullptr);
        break;
    case WM_TIMER:
        if (wp == kHoldTimer) {
            KillTimer(h, kHoldTimer);
            if (GetKeyState(VK_LBUTTON) < 0) {
                g_holdFired = true;
                // Release the button so it does not also fire a click.
                SendMessageW(h, BM_SETSTATE, FALSE, 0);
                ReleaseCapture();
                ShowTrackMenu(h, subs);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        KillTimer(h, kHoldTimer);
        if (g_holdFired) { g_holdFired = false; return 0; }
        break;
    case WM_RBUTTONUP:
        ShowTrackMenu(h, subs);
        return 0;
    }
    return CallWindowProcW(g_trackBtnOrigProc, h, msg, wp, lp);
}

static void ShowPlayingStatus() {
    if (g_plIndex >= 0 && g_plIndex < (int)g_playlist.size()) {
        wchar_t b[64];
        swprintf(b, 64, L"  [%d/%d]", g_plIndex + 1, (int)g_playlist.size());
        std::wstring s = L"Playing: " + g_playlist[g_plIndex].name;
        if (g_archiveSession && g_mpvPlCount > 0 && !g_mediaTitle.empty()) {
            wchar_t e[48];
            swprintf(e, 48, L" (%lld/%lld)", (long long)g_mpvPlPos + 1, (long long)g_mpvPlCount);
            s += L" \u203A " + g_mediaTitle + e;  // "Show.zip > Episode 01.mkv (1/2)"
        }
        SetStatus(s + b);
    }
}

// mpv property as a UTF-8 string ("" if unavailable).
static std::string MpvString(const char* name) {
    if (!g_mpvh || !g_mpv.get_property_string) return "";
    char* v = g_mpv.get_property_string(g_mpvh, name);
    std::string r = v ? v : "";
    if (v) g_mpv.free(v);
    return r;
}

// "archive://<archive url>|/<entry>" -> "<entry>"
static std::string ArchiveEntryOf(const std::string& path) {
    size_t bar = path.rfind("|/");
    return bar == std::string::npos ? path : path.substr(bar + 2);
}

// mpv lists every entry of an archive (sorted by name), including .nfo, .txt
// and pictures.  When the first entry starts, remove the ones that are not
// video / audio (from the end, so indexes stay valid), so the archive's
// entry count is right for next / previous / auto-advance.  Verified with mpv
// (zip, 7z, multi-part rar, local and over a seekable network stream).
static void FilterArchivePlaylist() {
    g_archiveFiltered = true;
    std::string cur = MpvString("playlist-pos");
    std::string cnt = MpvString("playlist-count");
    int pos = cur.empty() ? -1 : atoi(cur.c_str());
    int n = cnt.empty() ? 0 : atoi(cnt.c_str());
    int media = 0;
    for (int i = n - 1; i >= 0; --i) {
        std::string key = "playlist/" + std::to_string(i) + "/filename";
        std::wstring entry = Utf8ToWide(ArchiveEntryOf(MpvString(key.c_str())));
        if (IsMedia(entry)) { ++media; continue; }
        if (i == pos) continue;  // handled below
        std::string idx = std::to_string(i);
        const char* a[] = {"playlist-remove", idx.c_str(), nullptr};
        g_mpv.command(g_mpvh, a);
    }
    if (media == 0) {
        SetStatus(L"No video or audio files in " +
                  (g_plIndex >= 0 && g_plIndex < (int)g_playlist.size() ? g_playlist[g_plIndex].name : L"the archive"));
    }
    AppLog("archive: " + std::to_string(n) + " entries, " + std::to_string(media) + " video/audio");
    std::wstring curEntry = Utf8ToWide(ArchiveEntryOf(MpvString("path")));
    if (!IsMedia(curEntry)) {
        const char* a[] = {"playlist-remove", "current", nullptr};  // skips to the next entry
        g_mpv.command(g_mpvh, a);
    }
}
static void PlayIndex(int i);
static void PlayNext();
static void PlayPrev();
static void StopPlayback();
static void ApplyFullscreen(bool fs);

static bool InitMpv(std::string& err) {
    g_mpvh = g_mpv.create();
    if (!g_mpvh) { err = "mpv_create failed"; return false; }
    int64_t wid = (int64_t)(intptr_t)g_video;
    g_mpv.set_option(g_mpvh, "wid", MPV_FORMAT_INT64, &wid);
    struct { const char* k; const char* v; } opts[] = {
        {"config", "yes"},               // honour the user's mpv.conf / input.conf
        {"terminal", "no"},
        {"input-default-bindings", "yes"},
        {"input-vo-keyboard", "yes"},
        {"osc", "yes"},
        {"idle", "yes"},
        {"force-window", "no"},
        {"keep-open", "no"},
        {"volume", "100"},
        {"volume-max", "100"},
        {"cache", "yes"},
        {"demuxer-max-bytes", "256MiB"},
        {"demuxer-max-back-bytes", "64MiB"},
        {"demuxer-readahead-secs", "30"},
        {"demuxer-seekable-cache", "yes"},  // seeking back within the buffer is instant
        {"cache-pause-wait", "5"},          // after a stall, resume with 5 s buffered
        {"sub-auto", "fuzzy"},
        {"hr-seek", "yes"},              // precise seeks (wheel / arrows)
        {"hwdec", "auto-safe"},
        {"cursor-autohide", "1000"},
        {"screenshot-directory", "~~desktop/"},
    };
    for (auto& o : opts) g_mpv.set_option_string(g_mpvh, o.k, o.v);
    g_mpv.request_log_messages(g_mpvh, "warn");
    if (g_mpv.initialize(g_mpvh) < 0) {
        err = "mpv_initialize failed";
        g_mpv.terminate_destroy(g_mpvh);
        g_mpvh = nullptr;
        return false;
    }
    g_mpv.stream_cb_add_ro(g_mpvh, kScheme, nullptr, RsOpen);
    {
        const char* a[] = {"define-section", "swp", kBindings, "force", nullptr};
        g_mpv.command(g_mpvh, a);
        const char* b[] = {"enable-section", "swp", nullptr};
        g_mpv.command(g_mpvh, b);
    }
    // A user mpv.conf might change the volume: force it back.
    double vol = 100;
    g_mpv.set_property(g_mpvh, "volume", MPV_FORMAT_DOUBLE, &vol);
    int nomute = 0;
    g_mpv.set_property(g_mpvh, "mute", MPV_FORMAT_FLAG, &nomute);

    g_mpv.observe_property(g_mpvh, 0, "time-pos", MPV_FORMAT_DOUBLE);
    g_mpv.observe_property(g_mpvh, 0, "duration", MPV_FORMAT_DOUBLE);
    g_mpv.observe_property(g_mpvh, 0, "pause", MPV_FORMAT_FLAG);
    g_mpv.observe_property(g_mpvh, 0, "speed", MPV_FORMAT_DOUBLE);
    g_mpv.observe_property(g_mpvh, 0, "fullscreen", MPV_FORMAT_FLAG);
    g_mpv.observe_property(g_mpvh, 0, "volume", MPV_FORMAT_DOUBLE);
    g_mpv.observe_property(g_mpvh, 0, "mute", MPV_FORMAT_FLAG);
    g_mpv.observe_property(g_mpvh, 0, "loop-file", MPV_FORMAT_STRING);
    g_mpv.observe_property(g_mpvh, 0, "media-title", MPV_FORMAT_STRING);
    g_mpv.observe_property(g_mpvh, 0, "idle-active", MPV_FORMAT_FLAG);
    g_mpv.observe_property(g_mpvh, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    g_mpv.observe_property(g_mpvh, 0, "cache-buffering-state", MPV_FORMAT_INT64);
    g_mpv.observe_property(g_mpvh, 0, "track-list", MPV_FORMAT_NODE);
    g_mpv.observe_property(g_mpvh, 0, "playlist-pos", MPV_FORMAT_INT64);
    g_mpv.observe_property(g_mpvh, 0, "playlist-count", MPV_FORMAT_INT64);
    g_mpv.observe_property(g_mpvh, 0, "file-size", MPV_FORMAT_INT64);
    g_mpv.observe_property(g_mpvh, 0, "video-frame-info/picture-type", MPV_FORMAT_STRING);
    g_mpv.observe_property(g_mpvh, 0, "frame-drop-count", MPV_FORMAT_INT64);
    g_mpv.observe_property(g_mpvh, 0, "decoder-frame-drop-count", MPV_FORMAT_INT64);
    g_mpv.set_wakeup_callback(g_mpvh, [](void*) { PostMessageW(g_main, WM_MPV_WAKEUP, 0, 0); }, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Back buffer sized from the keyframe interval
// ---------------------------------------------------------------------------
static int64_t g_fileSize = 0;
static double g_lastIFrameTime = -1;   // time-pos of the last I-frame seen (-1 after a seek)
static int64_t g_backBytesSet = 0;
static const int64_t kMinBackBytes = 64LL * 1024 * 1024;
static const int64_t kMaxBackBytes = 1024LL * 1024 * 1024;

// mpv's back buffer (demuxed data kept behind the playing position) holds
// 5 s + one keyframe interval, so after stepping back 5 s the keyframe that a
// precise seek decodes from is still in memory.  Never below 64 MiB.
static void ApplyBackBuffer() {
    double kfi = KeyframeIntervalForBuffers();
    double rate = (g_fileSize > 0 && g_duration > 1.0) ? g_fileSize / g_duration : 0.0;
    int64_t want = (int64_t)((kBufferSecs + kfi) * rate * 1.5);  // x1.5: bitrate peaks
    want = std::max(kMinBackBytes, std::min(kMaxBackBytes, want));
    if (std::llabs(want - g_backBytesSet) < 1024 * 1024) return;
    g_backBytesSet = want;
    Cmd({"set", "demuxer-max-back-bytes", std::to_string(want)});
    char b[160];
    snprintf(b, sizeof(b), "back buffer: %.0f MiB (keyframe interval used %.2f s, measured %.2f s, %.1f MiB/s)",
             want / 1048576.0, kfi, g_keyframeInterval.load(), rate / 1048576.0);
    AppLog(b);
}

static int64_t g_dropCount = 0, g_dropCountAtIFrame = 0;  // frames dropped (vo + decoder)

static void ResetKeyframeInterval() {
    g_keyframeInterval = 0;
    g_lastIFrameTime = -1;
    g_fileSize = 0;
}

// Called on every change of video-frame-info/picture-type: the time between
// two I-frames (without a seek in between) is the keyframe interval.
static void OnPictureType(const char* type) {
    if (!type || strcmp(type, "I") != 0) return;
    double t = g_timePos;
    // A dropped frame may have been an I-frame: such a gap would be too long.
    bool dropped = g_dropCount != g_dropCountAtIFrame;
    if (g_lastIFrameTime >= 0 && !dropped) {
        double gap = t - g_lastIFrameTime;
        if (gap > 0.04 && gap <= 30.0 && gap > g_keyframeInterval.load() + 0.01) {
            g_keyframeInterval = gap;
            char b[96];
            snprintf(b, sizeof(b), "keyframe interval measured: %.2f s", gap);
            AppLog(b);
            ApplyBackBuffer();
        }
    }
    g_lastIFrameTime = t;
    g_dropCountAtIFrame = g_dropCount;
}

// ---------------------------------------------------------------------------
// Shared resume positions (Unix servers only)
//
// Playback positions of remote videos are kept in one XML file in the user's
// home folder on the server, so any device connecting to that server resumes
// where the last one stopped.  Every save re-reads the server file and merges
// (newest "updated" wins per file), so devices do not overwrite each other.
// Windows servers are left alone.
// ---------------------------------------------------------------------------
static const char* kResumeFileName = ".ssh-win-player-positions.xml";
static const UINT_PTR kResumeTimer = 78;
static const UINT kResumeTimerMs = 60000;       // periodic save while playing

static std::mutex g_resumeMtx;
static std::map<std::string, ResumeEntry> g_resumeCache;
static std::string g_resumeFile;
static std::string g_resumeLoadingPath, g_resumeCurPath;  // file being opened / playing
static double g_resumeCurPos = 0, g_resumeCurDur = 0, g_resumeLastSaved = -1;
static double g_resumeLoadingStart = 0;  // position the file being opened starts at

struct ResumeJob {
    std::string file, path;
    double pos = 0, duration = 0;
    bool remove = false;
    int64_t updated = 0;
};
static std::mutex g_resumeQMtx;
static std::condition_variable g_resumeQCv;
static std::deque<ResumeJob> g_resumeQueue;
static bool g_resumeBusy = false, g_resumeStop = false;
static std::thread g_resumeThread;



static std::vector<ResumeJob> g_resumeFailed;  // saves that failed (connection lost); guarded by g_resumeQMtx

static bool ProcessResumeJob(const ResumeJob& j) {
    std::string text, err;
    bool exists = false;
    if (!g_browseConn.readTextFile(j.file, text, exists, err)) {
        AppLog("resume: cannot read " + j.file + ": " + err);
        return false;
    }
    std::map<std::string, ResumeEntry> m;
    if (exists) ParseResumeXml(text, m);
    ApplyResumeChange(m, j.path, j.pos, j.duration, j.remove, j.updated);
    if (!g_browseConn.writeTextFile(j.file, BuildResumeXml(m), err)) {
        AppLog("resume: cannot write " + j.file + ": " + err);
        return false;
    }
    char b[64];
    snprintf(b, sizeof(b), j.remove ? "resume: cleared " : "resume: saved %.1f s ", j.pos);
    AppLog(b + j.path);
    std::lock_guard<std::mutex> lk(g_resumeMtx);
    auto c = g_resumeCache.find(j.path);
    if (c != g_resumeCache.end() && c->second.updated == j.updated) c->second.pending = false;
    return true;
}

static void ResumeWorker() {
    for (;;) {
        ResumeJob j;
        {
            std::unique_lock<std::mutex> lk(g_resumeQMtx);
            g_resumeQCv.wait(lk, [] { return g_resumeStop || !g_resumeQueue.empty(); });
            if (g_resumeQueue.empty()) break;  // stop requested and nothing left
            j = g_resumeQueue.front();
            g_resumeQueue.pop_front();
            g_resumeBusy = true;
        }
        bool ok = ProcessResumeJob(j);
        {
            std::lock_guard<std::mutex> lk(g_resumeQMtx);
            g_resumeBusy = false;
            if (!ok) {  // keep it (newest per file) and write it after reconnecting
                g_resumeFailed.erase(std::remove_if(g_resumeFailed.begin(), g_resumeFailed.end(),
                                                    [&](const ResumeJob& f) { return f.path == j.path && f.file == j.file; }),
                                     g_resumeFailed.end());
                g_resumeFailed.push_back(j);
            }
        }
        g_resumeQCv.notify_all();
    }
}

// Waits until queued saves are written (used before disconnecting / exit).
static bool WaitResumeIdle(DWORD ms) {
    std::unique_lock<std::mutex> lk(g_resumeQMtx);
    return g_resumeQCv.wait_for(lk, std::chrono::milliseconds(ms),
                                [] { return g_resumeQueue.empty() && !g_resumeBusy; });
}

static void QueueResume(const std::string& path, double pos, double dur, bool remove) {
    if (!g_resumeEnabled || path.empty()) return;
    ResumeJob j;
    j.file = g_resumeFile;
    j.path = path;
    j.pos = pos;
    j.duration = dur;
    j.remove = remove;
    j.updated = (int64_t)time(nullptr);
    {
        std::lock_guard<std::mutex> lk(g_resumeMtx);
        ResumeEntry& e = g_resumeCache[path];
        e.pos = pos;
        e.duration = dur;
        e.updated = j.updated;
        e.removed = remove;
        e.pending = true;
    }
    {
        std::lock_guard<std::mutex> lk(g_resumeQMtx);
        // Only the latest save of a file matters.
        g_resumeQueue.erase(std::remove_if(g_resumeQueue.begin(), g_resumeQueue.end(),
                                           [&](const ResumeJob& q) { return q.path == path; }),
                            g_resumeQueue.end());
        g_resumeQueue.push_back(j);
    }
    g_resumeQCv.notify_all();
}

// After (re)connecting: write the saves that failed while the connection was
// down, and keep them visible to ResumePositionFor in the meantime.
static void RequeueFailedResume() {
    std::vector<ResumeJob> jobs;
    {
        std::lock_guard<std::mutex> lk(g_resumeQMtx);
        for (auto it = g_resumeFailed.begin(); it != g_resumeFailed.end();) {
            if (it->file == g_resumeFile) { jobs.push_back(*it); it = g_resumeFailed.erase(it); }
            else ++it;
        }
    }
    if (jobs.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_resumeMtx);
        for (auto& j : jobs) {
            auto it = g_resumeCache.find(j.path);
            if (it != g_resumeCache.end() && it->second.updated > j.updated) continue;  // newer elsewhere
            ResumeEntry& e = g_resumeCache[j.path];
            e.pos = j.pos;
            e.duration = j.duration;
            e.updated = j.updated;
            e.removed = j.remove;
            e.pending = true;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_resumeQMtx);
        for (auto& j : jobs) g_resumeQueue.push_back(j);
    }
    g_resumeQCv.notify_all();
    AppLog("resume: retrying " + std::to_string(jobs.size()) + " save(s) from before the connection was lost");
}

// Re-reads the server file (positions saved by other devices) and merges it
// with local saves that are not written yet.
static void RefreshResumeCache() {
    std::string text, err;
    bool exists = false;
    std::map<std::string, ResumeEntry> server;
    if (!g_browseConn.readTextFile(g_resumeFile, text, exists, err)) {
        AppLog("resume: cannot read " + g_resumeFile + ": " + err);
        return;
    }
    if (exists) ParseResumeXml(text, server);
    std::lock_guard<std::mutex> lk(g_resumeMtx);
    g_resumeCache = MergePendingResume(std::move(server), g_resumeCache);
}

// Position to resume a remote file at (0 = from the start).
static double ResumePositionFor(const std::string& path) {
    if (!g_resumeEnabled) return 0;
    RefreshResumeCache();
    std::lock_guard<std::mutex> lk(g_resumeMtx);
    return ResumeStartFor(g_resumeCache, path);
}

// Save the position of the playing remote file (finished = reached the end).
static void ResumeSaveCurrent(bool finished) {
    if (g_resumeCurPath.empty()) return;
    double pos = g_resumeCurPos, dur = g_resumeCurDur > 0 ? g_resumeCurDur : g_duration;
    bool remove = finished || ResumeShouldForget(pos, dur);
    QueueResume(g_resumeCurPath, pos, dur, remove);
    g_resumeLastSaved = pos;
}

static void HandleMpvEvents() {
    while (g_mpvh) {
        mpv_event* ev = g_mpv.wait_event(g_mpvh, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* p = static_cast<mpv_event_property*>(ev->data);
            std::string name = p->name;
            if (name == "time-pos") {
                g_timePos = p->format == MPV_FORMAT_DOUBLE ? *(double*)p->data : 0;
                if (p->format == MPV_FORMAT_DOUBLE && !g_resumeCurPath.empty()) g_resumeCurPos = g_timePos;
            } else if (name == "duration") {
                g_duration = p->format == MPV_FORMAT_DOUBLE ? *(double*)p->data : 0;
                g_streamDuration = g_duration;  // lets the prefetcher size its 5 s windows
                if (p->format == MPV_FORMAT_DOUBLE && !g_resumeCurPath.empty()) g_resumeCurDur = g_duration;
                ApplyBackBuffer();
            } else if (name == "pause") {
                g_paused = p->format == MPV_FORMAT_FLAG && *(int*)p->data;
                // Pausing is a likely moment to continue on another device: save now.
                if (g_paused && !g_resumeCurPath.empty() && std::fabs(g_resumeCurPos - g_resumeLastSaved) >= 1.0)
                    ResumeSaveCurrent(false);
            } else if (name == "speed") {
                if (p->format == MPV_FORMAT_DOUBLE) g_speed = *(double*)p->data;
            } else if (name == "idle-active") {
                g_idle = p->format == MPV_FORMAT_FLAG && *(int*)p->data;
                if (g_idle) { g_timePos = 0; g_duration = 0; g_mediaTitle.clear(); g_streamDuration = 0; }
                InvalidateRect(g_video, nullptr, TRUE);
            } else if (name == "fullscreen") {
                if (p->format == MPV_FORMAT_FLAG) ApplyFullscreen(*(int*)p->data != 0);
            } else if (name == "volume") {
                if (p->format == MPV_FORMAT_DOUBLE && std::fabs(*(double*)p->data - 100.0) > 0.01) {
                    AppLog("volume locked: " + std::to_string(*(double*)p->data) + " -> 100");
                    SetPropDouble("volume", 100.0);
                }
            } else if (name == "mute") {
                if (p->format == MPV_FORMAT_FLAG && *(int*)p->data) SetPropFlag("mute", false);
            } else if (name == "loop-file") {
                bool loop = p->format == MPV_FORMAT_STRING && strcmp(*(char**)p->data, "no") != 0;
                SendMessageW(g_btn[IDC_BTN_LOOP - IDC_BTN_FIRST], BM_SETCHECK, loop ? BST_CHECKED : BST_UNCHECKED, 0);
            } else if (name == "paused-for-cache") {
                g_buffering = p->format == MPV_FORMAT_FLAG && *(int*)p->data;
                AppLog(g_buffering ? "buffering started" : "buffering done");
                if (g_buffering) SetStatus(L"Buffering...");
                else if (!g_idle) ShowPlayingStatus();
            } else if (name == "cache-buffering-state") {
                if (g_buffering && p->format == MPV_FORMAT_INT64)
                    SetStatus(L"Buffering... " + std::to_wstring(*(int64_t*)p->data) + L"%");
            } else if (name == "video-frame-info/picture-type") {
                OnPictureType(p->format == MPV_FORMAT_STRING ? *(char**)p->data : nullptr);
            } else if (name == "frame-drop-count" || name == "decoder-frame-drop-count") {
                static int64_t vo = 0, dec = 0;
                int64_t v = p->format == MPV_FORMAT_INT64 ? *(int64_t*)p->data : 0;
                (name == "frame-drop-count" ? vo : dec) = v;
                g_dropCount = vo + dec;
            } else if (name == "file-size") {
                g_fileSize = p->format == MPV_FORMAT_INT64 ? *(int64_t*)p->data : 0;
                ApplyBackBuffer();
            } else if (name == "track-list") {
                ParseTrackList(p->format == MPV_FORMAT_NODE ? static_cast<mpv_node*>(p->data) : nullptr);
            } else if (name == "playlist-pos") {
                g_mpvPlPos = p->format == MPV_FORMAT_INT64 ? *(int64_t*)p->data : -1;
            } else if (name == "playlist-count") {
                g_mpvPlCount = p->format == MPV_FORMAT_INT64 ? *(int64_t*)p->data : 0;
            } else if (name == "media-title") {
                g_mediaTitle = p->format == MPV_FORMAT_STRING ? Utf8ToWide(*(char**)p->data) : L"";
                InvalidateRect(g_video, nullptr, TRUE);
                if (g_archiveSession && !g_idle) ShowPlayingStatus();  // entry name inside the archive
            }
            break;
        }
        case MPV_EVENT_FILE_LOADED: {
            AppLog("file loaded");
            g_resumeCurPath = g_resumeLoadingPath;  // remote file whose position is tracked
            g_resumeLoadingPath.clear();
            // Until mpv reports a time, the file is at its start position: stopping
            // right away must not overwrite a saved position with 0.
            g_resumeCurPos = g_resumeLoadingStart;
            g_resumeLastSaved = g_resumeLoadingStart;
            g_resumeCurDur = g_duration;
            // Inside a remote archive: positions are kept per entry ("<archive>|/<entry>").
            if (g_archiveSession && g_resumeEnabled && g_remotePlayIndex >= 0 &&
                g_remotePlayIndex < (int)g_playlist.size()) {
                std::string path = MpvString("path");
                if (path.compare(0, 10, "archive://") == 0) {
                    g_resumeCurPath = g_playlist[g_remotePlayIndex].remotePath + "|/" + ArchiveEntryOf(path);
                    double at = g_archiveFromStart ? 0 : ResumePositionFor(g_resumeCurPath);
                    g_resumeCurPos = g_resumeLastSaved = at;
                    if (at > 0) {
                        char b[32];
                        snprintf(b, sizeof(b), "%.3f", at);
                        Cmd({"seek", b, "absolute"});
                        SetStatus(L"Resuming at " + FormatTime(at) + L": " + Utf8ToWide(ArchiveEntryOf(path)));
                    }
                }
            }
            // Select the first external subtitle; add the others as choices.
            for (size_t i = 0; i < g_pendingSubs.size(); ++i)
                Cmd({"sub-add", g_pendingSubs[i], i == 0 ? "select" : "auto"});
            g_pendingSubs.clear();
            ShowPlayingStatus();
            break;
        }
        case MPV_EVENT_END_FILE: {
            auto* e = static_cast<mpv_event_end_file*>(ev->data);
            // Remember where the file stopped (or forget it when it played to the end).
            ResumeSaveCurrent(e->reason == MPV_END_FILE_REASON_EOF);
            g_resumeCurPath.clear();
            if (e->reason == MPV_END_FILE_REASON_EOF && g_mpvPlPos >= 0 && g_mpvPlPos + 1 < g_mpvPlCount) {
                // More entries in the opened archive: mpv plays the next one itself.
            } else if (e->reason == MPV_END_FILE_REASON_EOF) {
                if (g_plIndex + 1 < (int)g_playlist.size()) PlayIndex(g_plIndex + 1);
                else { SetStatus(L"Finished"); g_remotePlayIndex = -1; }
            } else if (e->reason == MPV_END_FILE_REASON_ERROR) {
                std::wstring name = (g_plIndex >= 0 && g_plIndex < (int)g_playlist.size()) ? g_playlist[g_plIndex].name : L"";
                SetStatus(L"Cannot play " + name + L": " + Utf8ToWide(g_mpv.error_string(e->error)));
            }
            break;
        }
        case MPV_EVENT_CLIENT_MESSAGE: {
            auto* m = static_cast<mpv_event_client_message*>(ev->data);
            if (m->num_args >= 1) {
                std::string a = m->args[0];
                if (a == "swp-next") PlayNext();
                else if (a == "swp-prev") PlayPrev();
                else if (a == "swp-stop") StopPlayback();
                else if (a == "swp-help") OpenHotkeyHelp();
            }
            break;
        }
        case MPV_EVENT_START_FILE:
            if (g_archiveSession && !g_archiveFiltered && MpvString("path").compare(0, 10, "archive://") == 0)
                FilterArchivePlaylist();
            break;
        case MPV_EVENT_SEEK:
            AppLog("seek");
            g_lastIFrameTime = -1;  // do not measure a keyframe gap across a seek
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            AppLog("playback restart");
            break;
        case MPV_EVENT_COMMAND_REPLY: {
            auto it = g_pendingCmds.find(ev->reply_userdata);
            if (it != g_pendingCmds.end()) {
                if (ev->error < 0) AppLog("command failed: " + it->second + ": " + g_mpv.error_string(ev->error));
                else if (it->second.compare(0, 7, "sub-add") == 0) AppLog("ok: " + it->second);
                g_pendingCmds.erase(it);
            }
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto* m = static_cast<mpv_event_log_message*>(ev->data);
            std::string t = std::string("mpv[") + m->prefix + "] " + m->level + ": " + m->text;
            while (!t.empty() && t.back() == '\n') t.pop_back();
            AppLog(t);
            break;
        }
        case MPV_EVENT_SHUTDOWN: {
            // Something (e.g. a user input.conf binding) asked mpv to quit.
            // Re-create the player so the app keeps working.
            g_mpv.terminate_destroy(g_mpvh);
            g_mpvh = nullptr;
            g_idle = true;
            std::string err;
            if (!InitMpv(err)) SetStatus(L"Player error: " + Utf8ToWide(err));
            InvalidateRect(g_video, nullptr, TRUE);
            break;
        }
        default:
            break;
        }
    }
    UpdateUiFromState();
}

// ---------------------------------------------------------------------------
// Player actions
// ---------------------------------------------------------------------------
static std::string MpvQuote(const std::string& v) {
    // mpv option-list quoting: %<bytes>%<value>
    return "%" + std::to_string(v.size()) + "%" + v;
}

static void PlayIndex(int i) {
    if (i < 0 || i >= (int)g_playlist.size()) return;
    g_plIndex = i;
    const PlayItem& it = g_playlist[i];
    g_remotePlayIndex = it.remote ? i : -1;
    g_pendingSubs = it.subs;
    g_streamDuration = 0;  // unknown until mpv reports the new file's duration
    ResetKeyframeInterval();
    g_resumeLoadingPath.clear();
    g_resumeLoadingStart = 0;
    double resumeAt = 0;
    // Shift held (Shift+double-click / Shift+Enter): start from the beginning.
    bool fromStart = GetKeyState(VK_SHIFT) < 0;
    g_archiveSession = it.archive;
    g_archiveFiltered = false;
    g_archiveFromStart = fromStart;
    if (it.remote) {
        std::string title = WideToUtf8(it.name);
        // Start playback only once 5 s are buffered (cache-pause-wait).
        // Archives: no forced title (each entry shows its own name) and positions are
        // handled per entry at FILE_LOADED instead of start=.
        std::string opts = it.archive ? std::string("cache-pause-initial=yes")
                                      : "force-media-title=" + MpvQuote(title) + ",cache-pause-initial=yes";
        if (g_resumeEnabled && it.archive) {
            opts += ",resume-playback=no";
        } else if (g_resumeEnabled) {
            opts += ",resume-playback=no";  // the server's shared position decides, not mpv's local one
            g_resumeLoadingPath = it.remotePath;
            resumeAt = fromStart ? 0 : ResumePositionFor(it.remotePath);  // saved by this or another device
            g_resumeLoadingStart = resumeAt;
            if (resumeAt > 0) {
                char b[48];
                snprintf(b, sizeof(b), ",start=%.3f", resumeAt);
                opts += b;
            }
        }
        Cmd({"loadfile", it.url, "replace", "-1", opts});
    } else {
        Cmd({"loadfile", it.url, "replace"});
    }
    SetPropFlag("pause", false);
    SetStatus((resumeAt > 0 ? L"Resuming at " + FormatTime(resumeAt) + L": " : L"Opening: ") + it.name);
    std::wstring title = std::wstring(kAppName) + L" - " + it.name;
    SetWindowTextW(g_main, title.c_str());
}

// Next / previous: within an opened archive first, then the folder playlist.
static void PlayNext() {
    if (g_archiveSession && !g_idle && g_mpvPlPos >= 0 && g_mpvPlPos + 1 < g_mpvPlCount) { Cmd({"playlist-next"}); return; }
    if (g_plIndex + 1 < (int)g_playlist.size()) PlayIndex(g_plIndex + 1);
}
static void PlayPrev() {
    if (g_archiveSession && !g_idle && g_mpvPlPos > 0) { Cmd({"playlist-prev"}); return; }
    if (g_plIndex > 0) PlayIndex(g_plIndex - 1);
}

static void StopPlayback() {
    g_remotePlayIndex = -1;  // stopped on purpose: nothing to continue after a reconnect
    Cmd({"stop"});
    SetStatus(L"Stopped");
    SetWindowTextW(g_main, kAppName);
}

static void PlayLocalFiles(const std::vector<std::wstring>& files) {
    g_playlist.clear();
    for (auto& f : files) {
        PlayItem it;
        it.url = WideToUtf8(f);
        size_t s = f.find_last_of(L"\\/");
        it.name = s == std::wstring::npos ? f : f.substr(s + 1);
        it.archive = IsArchive(it.name);
        it.dir = s == std::wstring::npos ? L"" : f.substr(0, s);
        g_playlist.push_back(it);
    }
    if (!g_playlist.empty()) PlayIndex(0);
}

// ---------------------------------------------------------------------------
// Fullscreen / layout
// ---------------------------------------------------------------------------
static void Layout() {
    RECT rc;
    GetClientRect(g_main, &rc);
    int W = rc.right, H = rc.bottom;
    bool fs = g_fullscreen;

    auto vis = [](HWND h, bool v) { ShowWindow(h, v ? SW_SHOWNA : SW_HIDE); };
    HWND topCtl[] = {g_lblHost, g_host, g_lblPort, g_port, g_lblPass, g_pass, g_lblKey, g_key, g_keyBrowse,
                     g_connectBtn, g_localBtn};
    for (HWND h : topCtl) vis(h, !fs);
    vis(g_status, !fs);
    for (HWND h : g_btn) vis(h, !fs);
    vis(g_seek, !fs);
    vis(g_time, !fs);
    bool listVis = !fs && g_showList;
    for (HWND h : {g_upBtn, g_homeBtn, g_refreshBtn, g_path, g_allFiles, g_list}) vis(h, listVis);

    if (fs) {
        MoveWindow(g_video, 0, 0, W, H, TRUE);
        return;
    }

    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_status, &sr);
    int statusH = sr.bottom - sr.top;
    int parts[2] = {std::max(100, W - S(260)), -1};
    SendMessageW(g_status, SB_SETPARTS, 2, (LPARAM)parts);

    int pad = S(6), rowH = S(24), gap = S(4);
    // --- top connection bar
    int x = pad, y = pad;
    auto place = [&](HWND h, int w, int yoff = 0) { MoveWindow(h, x, y + yoff, w, rowH, TRUE); x += w + gap; };
    int lblOff = S(4);
    place(g_lblHost, S(32), lblOff);
    place(g_host, S(190));
    place(g_lblPort, S(30), lblOff);
    place(g_port, S(48));
    place(g_lblPass, S(62), lblOff);
    place(g_pass, S(120));
    place(g_lblKey, S(26), lblOff);
    place(g_key, S(140));
    place(g_keyBrowse, S(26));
    x += gap;
    place(g_connectBtn, S(90));
    place(g_localBtn, S(100));
    int top = y + rowH + pad;
    int bottom = H - statusH;

    // --- player controls (bottom of the right area)
    int ctlH = S(32);
    int seekH = S(26);
    int left = 0;
    if (g_showList) {
        int lw = std::max(S(150), std::min(g_listW, W - S(250)));
        int lx = pad, ly = top;
        MoveWindow(g_upBtn, lx, ly, S(30), rowH, TRUE);
        MoveWindow(g_homeBtn, lx + S(30) + gap, ly, S(30), rowH, TRUE);
        MoveWindow(g_refreshBtn, lx + 2 * (S(30) + gap), ly, S(30), rowH, TRUE);
        int px = lx + 3 * (S(30) + gap);
        MoveWindow(g_path, px, ly, lw - (px - lx), rowH, TRUE);
        ly += rowH + gap;
        MoveWindow(g_allFiles, lx, ly, lw, S(20), TRUE);
        ly += S(20) + gap;
        MoveWindow(g_list, lx, ly, lw, bottom - pad - ly, TRUE);
        int cw = lw - S(100) - GetSystemMetrics(SM_CXVSCROLL) - S(4);
        ListView_SetColumnWidth(g_list, 0, std::max(S(80), cw));
        ListView_SetColumnWidth(g_list, 1, S(100));
        left = lx + lw + S(6);  // splitter is the gap
    } else {
        left = pad;
    }
    int rw = W - left - pad;
    int ctlY = bottom - pad - ctlH;
    int seekY = ctlY - gap - seekH;
    MoveWindow(g_video, left, top, rw, std::max(10, seekY - gap - top), TRUE);
    int timeW = S(130);
    MoveWindow(g_seek, left, seekY, std::max(10, rw - timeW - gap), seekH, TRUE);
    MoveWindow(g_time, left + rw - timeW, seekY + S(4), timeW, seekH - S(4), TRUE);

    // Buttons: widths in units
    static const int widths[] = {34, 34, 44, 34, 44, 34, 34, 44, 34, 34, 26, 52, 26, 40, 46, 42, 74, 30, 56};
    static const bool gapAfter[] = {false, false, false, false, false, false, false, false, false, true,
                                    false, false, true, false, false, true, false, false, false};
    int bx = left;
    for (int i = 0; i < IDC_BTN_LAST - IDC_BTN_FIRST; ++i) {
        int w = S(widths[i]);
        MoveWindow(g_btn[i], bx, ctlY, w, ctlH, TRUE);
        bx += w + S(2) + (gapAfter[i] ? S(10) : 0);
    }
    InvalidateRect(g_main, nullptr, TRUE);
}

static void ApplyFullscreen(bool fs) {
    if (fs == g_fullscreen) return;
    g_fullscreen = fs;
    DWORD style = GetWindowLongW(g_main, GWL_STYLE);
    if (fs) {
        g_wpPrev.length = sizeof(g_wpPrev);
        GetWindowPlacement(g_main, &g_wpPrev);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(g_main, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_main, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        Layout();
        HWND mpvWnd = FindWindowExW(g_video, nullptr, nullptr, nullptr);
        SetFocus(mpvWnd ? mpvWnd : g_main);
    } else {
        SetWindowLongW(g_main, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_main, &g_wpPrev);
        SetWindowPos(g_main, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        Layout();
        SetFocus(g_main);
    }
}

static void UpdateSeekTip();
static void UpdateUiFromState() {
    UpdateSeekTip();  // hover tip shows the distance from the moving position
    static std::wstring lastTime, lastPlay, lastSpeed;
    std::wstring t = FormatTime(g_timePos) + L" / " + FormatTime(g_duration);
    if (g_idle) t = L"--:-- / --:--";
    if (t != lastTime) { SetWindowTextW(g_time, t.c_str()); lastTime = t; }
    std::wstring play = (g_paused || g_idle) ? L"\u25B6" : L"\u23F8";
    if (play != lastPlay) { SetWindowTextW(g_btn[IDC_BTN_PLAY - IDC_BTN_FIRST], play.c_str()); lastPlay = play; }
    wchar_t sp[32];
    swprintf(sp, 32, L"%.2f\u00D7", g_speed);
    if (lastSpeed != sp) { SetWindowTextW(g_btn[IDC_BTN_SPEED - IDC_BTN_FIRST], sp); lastSpeed = sp; }
    if (!g_seekDragging) {
        int pos = (g_duration > 0) ? (int)(g_timePos / g_duration * 10000.0) : 0;
        pos = std::max(0, std::min(10000, pos));
        if ((int)SendMessageW(g_seek, TBM_GETPOS, 0, 0) != pos) SendMessageW(g_seek, TBM_SETPOS, TRUE, pos);
    }
}

// ---------------------------------------------------------------------------
// File browser
// ---------------------------------------------------------------------------
static int IconIndex(const std::wstring& name, bool dir) {
    static std::map<std::wstring, int> cache;
    std::wstring key = dir ? L"<dir>" : LowerExt(name);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    SHFILEINFOW sfi{};
    std::wstring probe = dir ? L"folder" : (L"file." + key);
    SHGetFileInfoW(probe.c_str(), dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    cache[key] = sfi.iIcon;
    return sfi.iIcon;
}

// PowerShell (Windows) servers use "/C:/Users/me" internally; show and accept
// the familiar "C:\Users\me" (and "\\server\share") in the path box.
static std::wstring RemotePathForDisplay(const std::string& posix) {
    std::wstring w = Utf8ToWide(posix);
    if (!g_remoteWinPaths || w == L"/") return w;
    if (w.size() >= 2 && w[0] == L'/' && w[1] == L'/') {  // //server/share -> \\server\share
        std::replace(w.begin(), w.end(), L'/', L'\\');
        return w;
    }
    if (!w.empty() && w[0] == L'/') w.erase(0, 1);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    if (w.size() == 2 && w[1] == L':') w += L'\\';  // "C:" -> "C:\"
    return w;
}

static std::string RemotePathFromTyped(std::wstring t) {
    if (g_remoteWinPaths) {
        bool drive = t.size() >= 2 && iswalpha(t[0]) && t[1] == L':';
        bool unc = t.size() >= 2 && t[0] == L'\\' && t[1] == L'\\';
        if (drive || unc || t.find(L'\\') != std::wstring::npos) {
            std::replace(t.begin(), t.end(), L'\\', L'/');
            if (drive) t = L"/" + t;
            while (t.size() > 1 && t.back() == L'/' && !(t.size() == 3 && t[2] == L':')) t.pop_back();
        }
    }
    return WideToUtf8(t);
}

static void UpdatePathBox() {
    std::wstring p;
    if (g_mode == Mode::Remote) p = RemotePathForDisplay(g_remotePath);
    else p = g_localPath.empty() ? L"This PC" : g_localPath;
    SetWindowTextW(g_path, p.c_str());
    SetWindowTextW(g_localBtn, (g_mode == Mode::Remote || !g_connected) ? L"Local files" : L"Remote files");
}

static void SortEntries() {
    std::stable_sort(g_entries.begin(), g_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isUp != b.isUp) return a.isUp;
        if (a.isDir != b.isDir) return a.isDir;
        if (a.isDrive || b.isDrive) return a.name < b.name;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
}

static std::wstring CurrentDirKey() {
    return g_mode == Mode::Remote ? (L"R:" + Utf8ToWide(g_remotePath)) : (L"L:" + g_localPath);
}

static void FillList(const std::wstring& selectName = L"") {
    bool all = SendMessageW(g_allFiles, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_shown.clear();
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    int selIdx = -1;
    // Highlight the playing item if it is in this folder.
    std::wstring playing;
    if (g_plIndex >= 0 && g_plIndex < (int)g_playlist.size() && g_playlist[g_plIndex].dir == CurrentDirKey())
        playing = g_playlist[g_plIndex].name;
    for (int i = 0; i < (int)g_entries.size(); ++i) {
        const Entry& e = g_entries[i];
        if (!all && !e.isDir && !e.isUp && !IsPlayable(e.name)) continue;
        int row = (int)g_shown.size();
        g_shown.push_back(i);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        it.iItem = row;
        it.pszText = (LPWSTR)e.name.c_str();
        it.lParam = i;
        if (e.isDrive) {
            SHFILEINFOW sfi{};
            SHGetFileInfoW(e.raw.empty() ? e.name.c_str() : Utf8ToWide(e.raw).c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            it.iImage = sfi.iIcon;
        } else {
            it.iImage = IconIndex(e.name, e.isDir || e.isUp);
        }
        ListView_InsertItem(g_list, &it);
        std::wstring sz = FormatSize(e.size);
        ListView_SetItemText(g_list, row, 1, (LPWSTR)sz.c_str());
        if ((!selectName.empty() && e.name == selectName) || (selectName.empty() && !playing.empty() && e.name == playing))
            selIdx = row;
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    if (selIdx < 0 && !g_shown.empty()) selIdx = 0;
    if (selIdx >= 0) {
        ListView_SetItemState(g_list, selIdx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_list, selIdx, FALSE);
    }
    UpdatePathBox();
}

static bool NavigateRemote(const std::string& path, const std::wstring& selectName = L"") {
    std::string real, err;
    std::vector<RemoteEntry> items;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool ok = g_browseConn.list(path, real, items, err);
    SetCursor(old);
    if (!ok) {
        // A broken connection is reported by the "SSH connection reset" dialog
        // (SshConn's lost callback -> WM_SSH_LOST).
        SetStatus(Utf8ToWide(err));
        UpdatePathBox();  // restore the current folder in the path box
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    g_mode = Mode::Remote;
    g_remotePath = real;
    g_entries.clear();
    if (real != "/") { Entry up; up.name = L".."; up.isUp = true; g_entries.push_back(up); }
    for (auto& r : items) {
        Entry e;
        e.raw = r.name;
        e.name = Utf8ToWide(r.name);
        e.isDir = r.isDir;
        if (!r.isDir) e.size = r.size;
        g_entries.push_back(std::move(e));
    }
    SortEntries();
    FillList(selectName);
    SetStatus(std::to_wstring(items.size()) + L" items");
    IniSet(L"paths", g_connLabel.c_str(), Utf8ToWide(real));
    return true;
}

static bool NavigateLocal(const std::wstring& path, const std::wstring& selectName = L"") {
    std::vector<Entry> list;
    if (path.empty()) {
        wchar_t drives[512];
        DWORD n = GetLogicalDriveStringsW(511, drives);
        for (wchar_t* d = drives; d < drives + n && *d; d += wcslen(d) + 1) {
            Entry e;
            e.isDir = true;
            e.isDrive = true;
            e.raw = WideToUtf8(d);
            wchar_t label[MAX_PATH] = L"";
            UINT type = GetDriveTypeW(d);
            std::wstring name = d;
            if (type != DRIVE_REMOVABLE && type != DRIVE_CDROM &&
                GetVolumeInformationW(d, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) && label[0])
                name += std::wstring(L"  (") + label + L")";
            e.name = name;
            list.push_back(e);
        }
        // User folders for convenience.
        int ids[] = {CSIDL_MYVIDEO, CSIDL_MYMUSIC, CSIDL_PERSONAL};
        for (int id : ids) {
            wchar_t b[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, id, nullptr, 0, b))) {
                Entry e;
                e.isDir = true;
                e.isDrive = true;
                e.raw = WideToUtf8(b);
                e.name = b;
                list.push_back(e);
            }
        }
    } else {
        WIN32_FIND_DATAW fd;
        std::wstring pat = JoinLocal(path, L"*");
        HANDLE h = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            SetStatus(L"Cannot open folder " + path);
            UpdatePathBox();
            MessageBeep(MB_ICONWARNING);
            return false;
        }
        Entry up;
        up.name = L"..";
        up.isUp = true;
        list.push_back(up);
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            Entry e;
            e.name = n;
            e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!e.isDir) e.size = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            list.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_mode = Mode::Local;
    g_localPath = path;
    g_entries = std::move(list);
    SortEntries();
    FillList(selectName);
    SetStatus(std::to_wstring(g_entries.size()) + L" items");
    IniSet(L"main", L"localPath", path);
    return true;
}

static void GoUp() {
    if (g_mode == Mode::Remote) {
        if (g_remotePath == "/") return;
        std::string cur = g_remotePath;
        size_t s = cur.rfind('/');
        std::wstring child = Utf8ToWide(s == std::string::npos ? cur : cur.substr(s + 1));
        NavigateRemote(RemoteParent(g_remotePath), child);
    } else {
        if (g_localPath.empty()) return;
        std::wstring cur = g_localPath;
        while (cur.size() > 3 && (cur.back() == L'\\' || cur.back() == L'/')) cur.pop_back();
        size_t s = cur.find_last_of(L"\\/");
        std::wstring child = (s == std::wstring::npos || cur.size() <= 3) ? L"" : cur.substr(s + 1);
        NavigateLocal(LocalParent(g_localPath), child);
    }
}

static void Refresh() {
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    std::wstring name;
    if (sel >= 0 && sel < (int)g_shown.size()) name = g_entries[g_shown[sel]].name;
    if (g_mode == Mode::Remote) NavigateRemote(g_remotePath, name);
    else NavigateLocal(g_localPath, name);
}

// Home button: the Windows user folder locally.  On a server: ~ for Unix
// (uname exists), %USERPROFILE% for Windows (see DetectRemoteSystem), else "/".
static void GoHome() {
    if (g_mode == Mode::Remote && g_connected) {
        if (g_remoteHome.empty() || !NavigateRemote(g_remoteHome)) NavigateRemote("/");
        return;
    }
    wchar_t b[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, b))) NavigateLocal(b);
    else NavigateLocal(L"");
}

static void PlayFromListing(int entryIdx) {
    const Entry& sel = g_entries[entryIdx];
    std::wstring dirKey = CurrentDirKey();
    std::vector<int> items;
    if (IsPlayable(sel.name)) {
        // Folder playlist: media files and archives (only the first part of a RAR set).
        for (int i : g_shown) if (!g_entries[i].isDir && !g_entries[i].isUp && IsPlayable(g_entries[i].name)) items.push_back(i);
    } else {
        items.push_back(entryIdx);
    }
    g_playlist.clear();
    int start = 0;
    for (int idx : items) {
        const Entry& e = g_entries[idx];
        PlayItem it;
        it.name = e.name;
        it.dir = dirKey;
        it.archive = IsArchive(e.name);
        if (g_mode == Mode::Remote) {
            it.remote = true;
            it.remotePath = JoinRemote(g_remotePath, e.raw);
            it.url = RemoteUrl(it.remotePath);
            // External subtitles next to the file (same base name).
            std::wstring base = e.name;
            size_t dot = base.rfind(L'.');
            if (dot != std::wstring::npos) base = base.substr(0, dot);
            CharLowerBuffW(&base[0], (DWORD)base.size());
            for (const Entry& s : g_entries) {
                if (s.isDir || !IsSub(s.name)) continue;
                std::wstring ln = s.name;
                CharLowerBuffW(&ln[0], (DWORD)ln.size());
                if (ln.compare(0, base.size(), base) == 0) it.subs.push_back(RemoteUrl(JoinRemote(g_remotePath, s.raw)));
            }
        } else {
            it.url = WideToUtf8(JoinLocal(g_localPath, e.name));
        }
        if (idx == entryIdx) start = (int)g_playlist.size();
        g_playlist.push_back(std::move(it));
    }
    PlayIndex(start);
}

static void ActivateSelected() {
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_shown.size()) return;
    int idx = g_shown[sel];
    Entry e = g_entries[idx];
    if (e.isUp) { GoUp(); return; }
    if (e.isDir) {
        if (g_mode == Mode::Remote) NavigateRemote(JoinRemote(g_remotePath, e.raw));
        else if (e.isDrive) NavigateLocal(Utf8ToWide(e.raw));
        else NavigateLocal(JoinLocal(g_localPath, e.name));
        return;
    }
    PlayFromListing(idx);
}

static void NavigateTyped() {
    std::wstring p = Trim(GetText(g_path));
    if (g_mode == Mode::Remote) {
        if (p.empty()) p = L"/";
        NavigateRemote(RemotePathFromTyped(p));
    } else {
        if (p == L"This PC") p.clear();
        NavigateLocal(p);
    }
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
// waitForSaves = false when the connection is already broken: the current
// position is still queued, fails, and is written after reconnecting.
static void Disconnect(bool waitForSaves = true) {
    // Write the current position before the connection goes away.
    ResumeSaveCurrent(false);
    g_resumeCurPath.clear();
    g_resumeLoadingPath.clear();
    if (waitForSaves) WaitResumeIdle(5000);
    g_resumeEnabled = false;
    bool remotePlaying = g_plIndex >= 0 && g_plIndex < (int)g_playlist.size() && g_playlist[g_plIndex].remote;
    if (remotePlaying) StopPlayback();
    g_streamConn.disconnect();
    g_browseConn.disconnect();
    g_connected = false;
    g_remoteIsWindows = false;
    g_remoteIsUnix = false;
    g_remoteWinPaths = false;
    g_remoteHome.clear();
    g_remoteOs.clear();
    SetWindowTextW(g_connectBtn, L"Connect");
    SetConnStatus();
    if (g_mode == Mode::Remote) NavigateLocal(g_localPath);
    else UpdatePathBox();
}

// Which kind of server is this, and where does Home go?
//  - uname reports a Windows POSIX layer (Git Bash/MSYS2 "MINGW*"/"MSYS*",
//    Cygwin "CYGWIN*", busybox-w32 "Windows_NT"), or there is no uname:
//    try the server's %USERPROFILE% (converted with cygpath).
//  - otherwise uname exists => Unix: Home = ~ (cd; pwd -P).
//  - nothing found: "/".
static bool IsWindowsUname(const std::string& os) {
    for (const char* p : {"MINGW", "MSYS", "CYGWIN", "UWIN", "WINDOWS"})
        if (_strnicmp(os.c_str(), p, strlen(p)) == 0) return true;
    return false;
}

static void DetectRemoteSystem() {
    std::string os, e, home;
    bool hasUname = false, found = false;
    g_browseConn.systemName(os, hasUname, e);
    g_remoteIsWindows = false;
    g_remoteHome.clear();
    bool maybeWindows = !hasUname || IsWindowsUname(os);
    if (maybeWindows && g_browseConn.windowsProfile(home, found, e) && found) {
        g_remoteIsWindows = true;
        g_remoteHome = home;
    } else if (hasUname && g_browseConn.home(home, e)) {
        g_remoteHome = home;
    }
    if (g_remoteIsWindows) g_remoteOs = L"Windows";
    else g_remoteOs = Utf8ToWide(os);
    g_remoteIsUnix = hasUname && !IsWindowsUname(os) && !g_remoteIsWindows;
    AppLog("remote system: " + (hasUname ? (os.empty() ? std::string("?") : os) : std::string("no uname")) +
           (g_remoteIsWindows ? " (Windows)" : "") + ", home: " + (g_remoteHome.empty() ? std::string("/") : g_remoteHome));
}

static void Connect() {
    if (g_connected) { Disconnect(); return; }
    SshParams p;
    std::wstring sshOverride = IniGet(L"main", L"sshExe");
    p.sshExe = sshOverride.empty() ? FindSshExe() : sshOverride;
    p.dest = Trim(GetText(g_host));
    p.port = Trim(GetText(g_port));
    p.keyFile = Trim(GetText(g_key));
    p.password = GetText(g_pass);
    if (p.dest.empty()) {
        MessageBoxW(g_main, L"Enter a host, e.g. user@server or an alias from ~/.ssh/config.", kAppName, MB_ICONINFORMATION);
        SetFocus(g_host);
        return;
    }
    IniSet(L"main", L"host", p.dest);
    IniSet(L"main", L"port", p.port);
    IniSet(L"main", L"key", p.keyFile);

    // Remote shell that worked last time for this server (sh or PowerShell).
    std::wstring connKey = p.dest + (p.port.empty() ? L"" : (L":" + p.port));
    if (IniGet(L"backend", connKey.c_str()) == L"powershell") p.backend = kBackendPowerShell;
    g_browseConn.setParams(p);
    g_streamConn.setParams(p);
    SetStatus(L"Connecting to " + p.dest + L"...");
    UpdateWindow(g_status);
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::string err;
    bool ok = g_browseConn.connect(err);
    SetCursor(old);
    if (!ok) {
        SetStatus(L"Connection failed");
        MessageBoxW(g_main, Utf8ToWide(err).c_str(), L"Connection failed", MB_ICONERROR);
        return;
    }
    g_connected = true;
    g_connLabel = connKey;
    // Let the streaming connection start with the shell that just worked.
    p.backend = g_browseConn.activeBackend();
    g_streamConn.setParams(p);
    IniSet(L"backend", connKey.c_str(), p.backend == kBackendPowerShell ? L"powershell" : L"sh");
    DetectRemoteSystem();
    g_remoteWinPaths = g_remoteIsWindows && p.backend == kBackendPowerShell;
    // Shared resume positions: only for Unix servers, in the home folder there.
    g_resumeEnabled = g_remoteIsUnix && p.backend == kBackendSh && !g_remoteHome.empty() &&
                      IniGetInt(L"main", L"resume", 1) != 0;
    {
        std::lock_guard<std::mutex> lk(g_resumeMtx);
        g_resumeCache.clear();
    }
    if (g_resumeEnabled) {
        g_resumeFile = JoinRemote(g_remoteHome, kResumeFileName);
        RefreshResumeCache();
        RequeueFailedResume();
        std::lock_guard<std::mutex> lk(g_resumeMtx);
        AppLog("resume: using " + g_resumeFile + " (" + std::to_string(g_resumeCache.size()) + " saved positions)");
    } else {
        AppLog("resume: off for this server (only Unix servers keep shared positions)");
    }
    SetConnStatus();
    AppLog(std::string("remote shell: ") + (p.backend == kBackendPowerShell ? "PowerShell" : "sh"));
    SetWindowTextW(g_connectBtn, L"Disconnect");
    SetConnStatus();
    // Warm up the streaming connection in the background.
    std::thread([]() { std::string e; g_streamConn.connect(e); }).detach();

    std::wstring last = IniGet(L"paths", g_connLabel.c_str());
    if (last.empty() || !NavigateRemote(WideToUtf8(last))) GoHome();
}

// ---------------------------------------------------------------------------
// Lost connection
// ---------------------------------------------------------------------------
static bool g_lostDialogOpen = false;

// An established SSH connection broke and the automatic reconnect failed.
static void OnConnectionLost(const std::string& sshError) {
    if (!g_connected || g_lostDialogOpen) return;  // already handled / user disconnected
    g_lostDialogOpen = true;
    // Remember what was playing so Retry can continue it.
    // (mpv may already have given up on the file because its data stopped.)
    int playing = (g_remotePlayIndex >= 0 && g_remotePlayIndex < (int)g_playlist.size() &&
                   g_playlist[g_remotePlayIndex].remote) ? g_remotePlayIndex : -1;
    std::wstring host = g_connLabel;
    if (g_fullscreen) SetPropFlag("fullscreen", false);
    ApplyFullscreen(false);
    Disconnect(false);  // stop remote playback; do not wait for saves that cannot succeed now
    SetStatus(L"SSH connection reset");

    std::wstring text = L"SSH connection reset\n\nThe connection to " + host +
                        L" was lost and could not be re-established.";
    std::string e = sshError;
    while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
    if (!e.empty()) text += L"\n\n" + Utf8ToWide(e);
    text += playing >= 0 ? L"\n\nRetry reconnects and continues the video where it stopped."
                         : L"\n\nRetry reconnects to the server.";
    int answer = MessageBoxW(g_main, text.c_str(), L"SSH connection reset", MB_RETRYCANCEL | MB_ICONERROR);
    if (answer == IDRETRY) {
        Connect();  // shows its own message if the server is still unreachable
        if (g_connected && playing >= 0 && playing < (int)g_playlist.size()) PlayIndex(playing);
    }
    g_lostDialogOpen = false;
}

// ---------------------------------------------------------------------------
// Keyboard forwarding (mpv hotkeys)
// ---------------------------------------------------------------------------
static std::string VkName(UINT vk, bool extended) {
    switch (vk) {
    case VK_LEFT: return "LEFT";
    case VK_RIGHT: return "RIGHT";
    case VK_UP: return "UP";
    case VK_DOWN: return "DOWN";
    case VK_SPACE: return "SPACE";
    case VK_RETURN: return extended ? "KP_ENTER" : "ENTER";
    case VK_ESCAPE: return "ESC";
    case VK_BACK: return "BS";
    case VK_TAB: return "TAB";
    case VK_DELETE: return "DEL";
    case VK_INSERT: return "INS";
    case VK_HOME: return "HOME";
    case VK_END: return "END";
    case VK_PRIOR: return "PGUP";
    case VK_NEXT: return "PGDWN";
    case VK_PAUSE: return "PAUSE";
    case VK_PRINT: case VK_SNAPSHOT: return "PRINT";
    case VK_DECIMAL: return "KP_DEC";
    case VK_ADD: return "KP_ADD";
    case VK_SUBTRACT: return "KP_SUBTRACT";
    case VK_MULTIPLY: return "KP_MULTIPLY";
    case VK_DIVIDE: return "KP_DIVIDE";
    case VK_MEDIA_PLAY_PAUSE: return "PLAYPAUSE";
    case VK_MEDIA_STOP: return "STOP";
    case VK_MEDIA_NEXT_TRACK: return "NEXT";
    case VK_MEDIA_PREV_TRACK: return "PREV";
    case VK_APPS: return "MENU";
    }
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "KP" + std::to_string(vk - VK_NUMPAD0);
    return "";
}

static std::string Mods(bool shift, bool ctrl, bool alt) {
    std::string m;
    if (ctrl) m += "Ctrl+";
    if (alt) m += "Alt+";
    if (shift) m += "Shift+";
    return m;
}

// Returns true when the key was consumed.
static bool ForwardKeyDown(UINT vk, LPARAM lp) {
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (vk == VK_F4 && alt) return false;  // Alt+F4 closes the app
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
        vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)
        return false;
    if (vk == VK_VOLUME_UP || vk == VK_VOLUME_DOWN || vk == VK_VOLUME_MUTE) return false;  // system volume
    if (vk == VK_F1 && !ctrl && !alt && !shift) { OpenHotkeyHelp(); return true; }
    if (ctrl && vk == 'O') { SendMessageW(g_main, WM_COMMAND, IDC_BTN_OPEN, 0); return true; }
    if (ctrl && vk == 'L') { SetFocus(g_path); SendMessageW(g_path, EM_SETSEL, 0, -1); return true; }
    std::string name = VkName(vk, (lp & (1 << 24)) != 0);
    if (!name.empty()) {
        SendKey(Mods(shift, ctrl, alt) + name);
        return true;
    }
    if (ctrl || alt) {
        UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
        if (!ch) return false;
        std::string k;
        if (ch >= 'A' && ch <= 'Z') {
            k = std::string(1, shift ? (char)ch : (char)(ch - 'A' + 'a'));
            shift = false;  // case already encodes Shift
        } else if (ch < 128) {
            k = std::string(1, (char)ch);
            if (k == "#") k = "SHARP";
        } else {
            return false;
        }
        SendKey(Mods(shift, ctrl, alt) + k);
        return true;
    }
    return false;  // printable: handled via WM_CHAR
}

static void ForwardChar(wchar_t ch) {
    if (ch < 32 || ch == 127) return;
    std::string s;
    if (ch == L'#') s = "SHARP";
    else if (ch == L' ') s = "SPACE";
    else s = WideToUtf8(std::wstring(1, ch));
    SendKey(s);
}

static bool IsEditWindow(HWND h) {
    wchar_t cls[32];
    GetClassNameW(h, cls, 32);
    return _wcsicmp(cls, L"Edit") == 0;
}

static bool BelongsToMain(HWND h) { return h == g_main || IsChild(g_main, h); }

static void HandleWheel(int delta) {
    g_wheelAccum += delta;
    bool shift = GetKeyState(VK_SHIFT) < 0;
    while (std::abs(g_wheelAccum) >= WHEEL_DELTA) {
        int dir = g_wheelAccum > 0 ? 1 : -1;
        g_wheelAccum -= dir * WHEEL_DELTA;
        SeekRel(dir * (shift ? kWheelSeekShift : kWheelSeek));
    }
}

// Pre-dispatch filter for the message loop.  Returns true when handled.
static bool PreTranslate(MSG& m) {
    if (!BelongsToMain(m.hwnd)) return false;
    if (m.message == WM_MOUSEWHEEL) {
        POINT pt = {GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)};
        HWND under = WindowFromPoint(pt);
        if (under == g_list || IsChild(g_list, under)) {
            if (m.hwnd != g_list) { SendMessageW(g_list, WM_MOUSEWHEEL, m.wParam, m.lParam); return true; }
            return false;
        }
        HandleWheel(GET_WHEEL_DELTA_WPARAM(m.wParam));
        return true;
    }
    if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
        UINT vk = (UINT)m.wParam;
        if (IsEditWindow(m.hwnd)) {
            if (vk == VK_RETURN) {
                if (m.hwnd == g_path) { NavigateTyped(); return true; }
                if (m.hwnd == g_host || m.hwnd == g_pass || m.hwnd == g_port || m.hwnd == g_key) {
                    if (!g_connected) Connect();
                    return true;
                }
            }
            if (vk == VK_ESCAPE) { SetFocus(g_main); return true; }
            return false;
        }
        if (m.hwnd == g_list) {
            switch (vk) {
            case VK_RETURN: ActivateSelected(); return true;
            case VK_BACK: GoUp(); return true;
            case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
                if (GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_CONTROL) >= 0) return false;
                break;
            }
        }
        if (m.hwnd == g_list && vk == VK_TAB) return false;
        return ForwardKeyDown(vk, m.lParam);
    }
    if (m.message == WM_CHAR && !IsEditWindow(m.hwnd)) {
        ForwardChar((wchar_t)m.wParam);
        return true;
    }
    if (m.message == WM_SYSCHAR && !IsEditWindow(m.hwnd)) return true;  // no beep for Alt+key
    return false;
}

// ---------------------------------------------------------------------------
// Window procedures
// ---------------------------------------------------------------------------
static WNDPROC g_seekOrigProc;

// Position on the seek bar (0..1) for a client x coordinate.
static double SeekFracFromX(HWND h, int x) {
    RECT ch, th;
    SendMessageW(h, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
    SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
    int tw = (th.right - th.left) / 2;
    int lo = ch.left + tw, hi = ch.right - tw;
    double frac = hi > lo ? (double)(x - lo) / (hi - lo) : 0;
    return std::max(0.0, std::min(1.0, frac));
}

// Hover tip on the seek bar: the time under the cursor and how far that is
// from the current position, e.g. "12:34  (+2:10)".  Follows the cursor and
// updates live while playing.
static HWND g_seekTip;
static bool g_seekTipActive = false;
static int g_seekTipX = 0;
static wchar_t g_seekTipText[96] = L"";

static void UpdateSeekTip() {
    if (!g_seekTipActive) return;
    std::wstring text;
    if (g_idle || g_duration <= 0) {
        text = L"Click or drag to seek  \u2022  wheel \u00B15 s";
    } else {
        double t = SeekFracFromX(g_seek, g_seekTipX) * g_duration;
        double d = t - g_timePos;
        text = FormatTime(t) + L"  (" + (d < 0 ? L"\u2212" : L"+") + FormatTime(std::fabs(d)) + L")";
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = g_seek;
    ti.uId = 1;
    if (text != g_seekTipText) {
        wcsncpy(g_seekTipText, text.c_str(), 95);
        g_seekTipText[95] = 0;
        ti.lpszText = g_seekTipText;
        SendMessageW(g_seekTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
    // Centre the tip above the cursor, just over the bar.
    RECT br;
    GetWindowRect(g_seek, &br);
    POINT pt = {g_seekTipX, 0};
    ClientToScreen(g_seek, &pt);
    DWORD sz = (DWORD)SendMessageW(g_seekTip, TTM_GETBUBBLESIZE, 0, (LPARAM)&ti);
    int w = LOWORD(sz), hgt = HIWORD(sz);
    SendMessageW(g_seekTip, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x - w / 2, br.top - hgt - S(2)));
}

static void ShowSeekTip(bool on) {
    if (on == g_seekTipActive) return;
    g_seekTipActive = on;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = g_seek;
    ti.uId = 1;
    if (on) UpdateSeekTip();
    SendMessageW(g_seekTip, TTM_TRACKACTIVATE, on ? TRUE : FALSE, (LPARAM)&ti);
    if (on) UpdateSeekTip();  // position again now that the size is known
}

static LRESULT CALLBACK SeekProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEMOVE) {
        g_seekTipX = GET_X_LPARAM(lp);
        if (!g_seekTipActive) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            ShowSeekTip(true);
        } else {
            UpdateSeekTip();
        }
    } else if (msg == WM_MOUSELEAVE) {
        if (GetCapture() != h) ShowSeekTip(false);  // keep it while dragging the thumb
    } else if (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) {
        POINT pt;
        GetCursorPos(&pt);
        RECT r;
        GetWindowRect(h, &r);
        if (!PtInRect(&r, pt)) ShowSeekTip(false);
    }
    if (msg == WM_LBUTTONDOWN) {
        // Jump directly to the clicked position (instead of paging).
        RECT th;
        SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
        int x = GET_X_LPARAM(lp);
        if (!(x >= th.left && x <= th.right)) {
            double frac = SeekFracFromX(h, x);
            SendMessageW(h, TBM_SETPOS, TRUE, (LPARAM)(frac * 10000));
            if (!g_idle) {
                char b[32];
                snprintf(b, sizeof(b), "%.3f", frac * 100.0);
                Cmd({"seek", b, "absolute-percent"});
            }
        }
    }
    return CallWindowProcW(g_seekOrigProc, h, msg, wp, lp);
}

static LRESULT CALLBACK VideoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        if (g_idle) {
            SetTextColor(dc, RGB(150, 150, 150));
            HGDIOBJ of = SelectObject(dc, g_font);
            const wchar_t* hint =
                L"Connect to a server (or use Local files) and double-click a video or audio file.\n"
                L"You can also drop files here.\n\n"
                L"Mouse wheel: seek \u00B15 s (Shift: \u00B130 s)  \u2022  mpv hotkeys work  \u2022  "
                L"Double-click / f: fullscreen  \u2022  F1: list of hotkeys";
            RECT tr = rc;
            InflateRect(&tr, -S(20), -S(20));
            RECT calc = tr;
            DrawTextW(dc, hint, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            int hgt = calc.bottom - calc.top;
            tr.top = (rc.bottom - hgt) / 2;
            tr.bottom = tr.top + hgt;
            DrawTextW(dc, hint, -1, &tr, DT_CENTER | DT_WORDBREAK);
            SelectObject(dc, of);
        } else {
            // Audio-only files: mpv creates no window, show the title instead.
            SetTextColor(dc, RGB(200, 200, 200));
            HGDIOBJ of = SelectObject(dc, g_bigFont);
            std::wstring t = L"\u266A\n" + g_mediaTitle;
            RECT tr = rc;
            InflateRect(&tr, -S(20), -S(20));
            RECT calc = tr;
            DrawTextW(dc, t.c_str(), -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            int hgt = calc.bottom - calc.top;
            tr.top = (rc.bottom - hgt) / 2;
            tr.bottom = tr.top + hgt;
            DrawTextW(dc, t.c_str(), -1, &tr, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(g_main);
        return 0;
    case WM_LBUTTONDBLCLK:
        SetPropFlag("fullscreen", !g_fullscreen);
        return 0;
    case WM_RBUTTONUP:
        if (!g_idle) Cmd({"cycle", "pause"});
        return 0;
    case WM_SIZE:
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_main,
                             (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

static void AddTip(HWND ctl, const wchar_t* text) {
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_main;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void CreateControls() {
    g_lblHost = MakeCtl(L"STATIC", L"Host", SS_RIGHT, IDC_HOST_LBL);
    g_host = MakeCtl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_HOST, WS_EX_CLIENTEDGE);
    g_lblPort = MakeCtl(L"STATIC", L"Port", SS_RIGHT, IDC_PORT_LBL);
    g_port = MakeCtl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, IDC_PORT, WS_EX_CLIENTEDGE);
    g_lblPass = MakeCtl(L"STATIC", L"Password", SS_RIGHT, IDC_PASS_LBL);
    g_pass = MakeCtl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, IDC_PASS, WS_EX_CLIENTEDGE);
    g_lblKey = MakeCtl(L"STATIC", L"Key", SS_RIGHT, IDC_KEY_LBL);
    g_key = MakeCtl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_KEY, WS_EX_CLIENTEDGE);
    g_keyBrowse = MakeCtl(L"BUTTON", L"...", WS_TABSTOP, IDC_KEYBROWSE);
    g_connectBtn = MakeCtl(L"BUTTON", L"Connect", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_CONNECT);
    g_localBtn = MakeCtl(L"BUTTON", L"Local files", WS_TABSTOP, IDC_LOCAL);

    g_upBtn = MakeCtl(L"BUTTON", L"\u2191", 0, IDC_UP);
    SendMessageW(g_upBtn, WM_SETFONT, (WPARAM)g_symFont, TRUE);
    g_homeBtn = MakeCtl(L"BUTTON", L"\u2302", 0, IDC_HOME);
    SendMessageW(g_homeBtn, WM_SETFONT, (WPARAM)g_symFont, TRUE);
    g_refreshBtn = MakeCtl(L"BUTTON", L"\u27F3", 0, IDC_REFRESH);
    SendMessageW(g_refreshBtn, WM_SETFONT, (WPARAM)g_symFont, TRUE);
    g_path = MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL, IDC_PATH, WS_EX_CLIENTEDGE);
    g_allFiles = MakeCtl(L"BUTTON", L"Show all files (not only media)", BS_AUTOCHECKBOX, IDC_ALLFILES);
    g_list = MakeCtl(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                     IDC_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    {
        SHFILEINFOW sfi{};
        HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        ListView_SetImageList(g_list, il, LVSIL_SMALL);
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)L"Name";
        c.cx = 200;
        ListView_InsertColumn(g_list, 0, &c);
        c.pszText = (LPWSTR)L"Size";
        c.cx = 80;
        c.mask |= LVCF_FMT;
        c.fmt = LVCFMT_RIGHT;
        ListView_InsertColumn(g_list, 1, &c);
    }

    WNDCLASSW vc{};
    vc.lpfnWndProc = VideoProc;
    vc.hInstance = g_inst;
    vc.lpszClassName = kVideoClass;
    vc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    vc.style = CS_DBLCLKS;
    RegisterClassW(&vc);
    g_video = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, g_main,
                              (HMENU)IDC_VIDEO, g_inst, nullptr);

    g_seek = MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | TBS_BOTH, IDC_SEEK);
    SendMessageW(g_seek, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(g_seek, TBM_SETRANGEMAX, TRUE, 10000);
    SendMessageW(g_seek, TBM_SETPAGESIZE, 0, 100);
    g_seekOrigProc = (WNDPROC)SetWindowLongPtrW(g_seek, GWLP_WNDPROC, (LONG_PTR)SeekProc);
    g_time = MakeCtl(L"STATIC", L"--:-- / --:--", SS_CENTER, IDC_TIME);

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, g_main,
                               (HMENU)IDC_STATUS, g_inst, nullptr);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);

    struct B { const wchar_t* text; const wchar_t* tip; bool sym; DWORD style; };
    static const B btns[] = {
        {L"\u23EE", L"Previous file in folder  ( < )", true, 0},
        {L"\u23EA", L"Back 1 minute  ( Down )", true, 0},
        {L"-10s", L"Back 10 seconds  ( Left = 5 s, wheel down )", false, 0},
        {L"\u25C2\u2759", L"Previous frame  ( , )", true, 0},
        {L"\u25B6", L"Play / Pause  ( Space, p, right click )", true, 0},
        {L"\u23F9", L"Stop  ( q )", true, 0},
        {L"\u2759\u25B8", L"Next frame  ( . )", true, 0},
        {L"+10s", L"Forward 10 seconds  ( Right = 5 s, wheel up )", false, 0},
        {L"\u23E9", L"Forward 1 minute  ( Up )", true, 0},
        {L"\u23ED", L"Next file in folder  ( > or Enter )", true, 0},
        {L"\u2212", L"Slower  ( [ )", true, 0},
        {L"1.00\u00D7", L"Playback speed - click to reset  ( Backspace )", false, 0},
        {L"+", L"Faster  ( ] )", true, 0},
        {L"Subs", L"Subtitles: click = next ( j ),  hold or right-click = choose from list", false, 0},
        {L"Audio", L"Audio track: click = next ( # ),  hold or right-click = choose from list", false, 0},
        {L"Loop", L"Loop current file  ( L )", false, BS_AUTOCHECKBOX | BS_PUSHLIKE},
        {L"Fullscreen", L"Fullscreen  ( f, double click, Esc to leave )", false, 0},
        {L"\u2630", L"Show / hide the file browser", true, 0},
        {L"Open...", L"Open local files  ( Ctrl+O )", false, 0},
    };
    for (int i = 0; i < IDC_BTN_LAST - IDC_BTN_FIRST; ++i) {
        g_btn[i] = MakeCtl(L"BUTTON", btns[i].text, btns[i].style, IDC_BTN_FIRST + i);
        if (btns[i].sym) SendMessageW(g_btn[i], WM_SETFONT, (WPARAM)g_symFont, TRUE);
    }

    g_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_main, nullptr, g_inst, nullptr);
    SendMessageW(g_tooltip, TTM_SETMAXTIPWIDTH, 0, 400);
    for (int i = 0; i < IDC_BTN_LAST - IDC_BTN_FIRST; ++i) AddTip(g_btn[i], btns[i].tip);
    g_trackBtnOrigProc = (WNDPROC)SetWindowLongPtrW(g_btn[IDC_BTN_SUB - IDC_BTN_FIRST], GWLP_WNDPROC, (LONG_PTR)TrackBtnProc);
    SetWindowLongPtrW(g_btn[IDC_BTN_AUDIO - IDC_BTN_FIRST], GWLP_WNDPROC, (LONG_PTR)TrackBtnProc);
    AddTip(g_host, L"[user@]host, or a Host alias from %USERPROFILE%\\.ssh\\config");
    AddTip(g_port, L"SSH port (empty = 22 / from ssh config)");
    AddTip(g_pass, L"Optional. Leave empty to use keys / ssh-agent. Also used as key passphrase. Never saved.");
    AddTip(g_key, L"Optional private key file (default: keys from %USERPROFILE%\\.ssh)");
    AddTip(g_keyBrowse, L"Choose a private key file");
    AddTip(g_upBtn, L"Parent folder  ( Backspace )");
    AddTip(g_refreshBtn, L"Refresh");
    AddTip(g_homeBtn, L"Home folder: your Windows user folder; on the server ~ (Unix) or %USERPROFILE% (Windows)");
    AddTip(g_localBtn, L"Switch between this PC and the remote server");
    g_seekTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_main, nullptr, g_inst, nullptr);
    {
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        ti.hwnd = g_seek;
        ti.uId = 1;
        ti.lpszText = g_seekTipText;
        SendMessageW(g_seekTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }

    SetWindowTextW(g_host, IniGet(L"main", L"host").c_str());
    SetWindowTextW(g_port, IniGet(L"main", L"port").c_str());
    SetWindowTextW(g_key, IniGet(L"main", L"key").c_str());
    SendMessageW(g_allFiles, BM_SETCHECK, IniGetInt(L"main", L"showAll", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_host, EM_SETCUEBANNER, TRUE, (LPARAM)L"user@server");
    SendMessageW(g_pass, EM_SETCUEBANNER, TRUE, (LPARAM)L"(optional)");
    SendMessageW(g_key, EM_SETCUEBANNER, TRUE, (LPARAM)L"(optional)");
    SendMessageW(g_port, EM_SETCUEBANNER, TRUE, (LPARAM)L"22");
}

static void OpenFilesDialog() {
    std::vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter =
        L"Media files\0*.mkv;*.mp4;*.m4v;*.avi;*.mov;*.wmv;*.flv;*.webm;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.vob;*.ogv;*.3gp;"
        L"*.mp3;*.flac;*.wav;*.ogg;*.opus;*.m4a;*.m4b;*.aac;*.wma;*.ape;*.wv;*.mka;*.aiff;*.dsf;*.ac3;*.dts;"
        L"*.zip;*.rar;*.7z\0"
        L"Archives (zip, rar, 7z)\0*.zip;*.rar;*.7z\0"
        L"All files\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::wstring> files;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) files.push_back(first);
    else {
        for (; *p; p += wcslen(p) + 1) files.push_back(JoinLocal(first, p));
    }
    PlayLocalFiles(files);
}

static void OnCommand(int id, int code) {
    switch (id) {
    case IDC_CONNECT: Connect(); break;
    case IDC_LOCAL:
        if (g_mode == Mode::Remote || !g_connected) NavigateLocal(g_localPath);
        else NavigateRemote(g_remotePath.empty() ? "/" : g_remotePath);
        break;
    case IDC_KEYBROWSE: {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_main;
        ofn.lpstrFilter = L"All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_key, file);
        break;
    }
    case IDC_UP: GoUp(); break;
    case IDC_REFRESH: Refresh(); break;
    case IDC_HOME: GoHome(); break;
    case IDC_ALLFILES:
        if (code == BN_CLICKED) {
            IniSet(L"main", L"showAll", SendMessageW(g_allFiles, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
            FillList();
        }
        break;
    case IDC_BTN_PREV: PlayPrev(); break;
    case IDC_BTN_NEXT: PlayNext(); break;
    case IDC_BTN_BACK60: SeekRel(-60); break;
    case IDC_BTN_BACK10: SeekRel(-10); break;
    case IDC_BTN_FWD10: SeekRel(10); break;
    case IDC_BTN_FWD60: SeekRel(60); break;
    case IDC_BTN_FRAMEBACK: Cmd({"frame-back-step"}); break;
    case IDC_BTN_FRAMESTEP: Cmd({"frame-step"}); break;
    case IDC_BTN_PLAY:
        if (g_idle) {
            if (g_plIndex >= 0 && g_plIndex < (int)g_playlist.size()) PlayIndex(g_plIndex);
            else ActivateSelected();
        } else {
            Cmd({"cycle", "pause"});
        }
        break;
    case IDC_BTN_STOP: StopPlayback(); break;
    case IDC_BTN_SLOWER: Cmd({"multiply", "speed", "0.9091"}); break;
    case IDC_BTN_FASTER: Cmd({"multiply", "speed", "1.1"}); break;
    case IDC_BTN_SPEED: SetPropDouble("speed", 1.0); break;
    case IDC_BTN_SUB: Cmd({"cycle", "sub"}); break;
    case IDC_BTN_AUDIO: Cmd({"cycle", "audio"}); break;
    case IDC_BTN_LOOP: {
        bool on = SendMessageW(g_btn[IDC_BTN_LOOP - IDC_BTN_FIRST], BM_GETCHECK, 0, 0) == BST_CHECKED;
        Cmd({"set", "loop-file", on ? "inf" : "no"});
        break;
    }
    case IDC_BTN_FULL: SetPropFlag("fullscreen", !g_fullscreen); break;
    case IDC_BTN_LIST:
        g_showList = !g_showList;
        Layout();
        break;
    case IDC_BTN_OPEN: OpenFilesDialog(); break;
    default: return;
    }
    // Keep keyboard focus off the buttons so hotkeys keep working.
    if (id >= IDC_BTN_FIRST && id < IDC_BTN_LAST) SetFocus(g_main);
}

static int SplitterX() {
    RECT r;
    GetWindowRect(g_list, &r);
    POINT p = {r.right, r.top};
    ScreenToClient(g_main, &p);
    return p.x;
}

static LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_main = h;
        CreateControls();
        DragAcceptFiles(h, TRUE);
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(640);
        mm->ptMinTrackSize.y = S(400);
        return 0;
    }
    case WM_COMMAND:
        OnCommand(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)lp;
        if (nm->hwndFrom == g_list && nm->code == NM_DBLCLK) ActivateSelected();
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == g_seek) {
            int code = LOWORD(wp);
            int pos = (int)SendMessageW(g_seek, TBM_GETPOS, 0, 0);
            char b[32];
            snprintf(b, sizeof(b), "%.3f", pos / 100.0);
            if (code == TB_THUMBTRACK) {
                g_seekDragging = true;
                if (GetTickCount() - g_lastDragSeek > 120 && !g_idle) {
                    g_lastDragSeek = GetTickCount();
                    Cmd({"seek", b, "absolute-percent+keyframes"});
                }
            } else if (code == TB_ENDTRACK || code == TB_THUMBPOSITION) {
                if (g_seekDragging && !g_idle) Cmd({"seek", b, "absolute-percent"});
                g_seekDragging = false;
                SetFocus(g_main);
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        break;
    case WM_SETCURSOR:
        if ((HWND)wp == h && LOWORD(lp) == HTCLIENT && g_showList && !g_fullscreen) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            int sx = SplitterX();
            if (p.x >= sx && p.x <= sx + S(6)) { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        int x = GET_X_LPARAM(lp);
        int sx = SplitterX();
        if (g_showList && !g_fullscreen && x >= sx && x <= sx + S(6)) {
            g_splitDrag = true;
            SetCapture(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_splitDrag) {
            g_listW = std::max(S(150), GET_X_LPARAM(lp) - S(6));
            Layout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_splitDrag) { g_splitDrag = false; ReleaseCapture(); }
        return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        for (UINT i = 0; i < n; ++i) {
            wchar_t f[32768];
            DragQueryFileW(d, i, f, 32768);
            DWORD a = GetFileAttributesW(f);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                if (n == 1) NavigateLocal(f);
                continue;
            }
            files.push_back(f);
        }
        DragFinish(d);
        std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
        });
        if (!files.empty()) PlayLocalFiles(files);
        return 0;
    }
    case WM_TIMER:
        if (wp == kResumeTimer && !g_resumeCurPath.empty() && !g_paused && !g_idle &&
            std::fabs(g_resumeCurPos - g_resumeLastSaved) >= 5.0)
            ResumeSaveCurrent(false);  // periodic save while playing
        return 0;
    case WM_SSH_LOST: {
        std::unique_ptr<std::string> e(reinterpret_cast<std::string*>(lp));
        OnConnectionLost(e ? *e : std::string());
        return 0;
    }
    case WM_MPV_WAKEUP:
        HandleMpvEvents();
        return 0;
    case WM_CLOSE: {
        IniSet(L"main", L"listWidth", std::to_wstring((int)(g_listW / g_scale)));
        IniSet(L"main", L"showList", g_showList ? L"1" : L"0");
        if (!g_fullscreen) {
            WINDOWPLACEMENT wp2 = {sizeof(wp2)};
            GetWindowPlacement(h, &wp2);
            RECT& r = wp2.rcNormalPosition;
            IniSet(L"main", L"win", std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," +
                                        std::to_wstring(r.right) + L"," + std::to_wstring(r.bottom) + L"," +
                                        (wp2.showCmd == SW_SHOWMAXIMIZED ? L"1" : L"0"));
        }
        // Save the position of the playing remote file, then stop the saver.
        ResumeSaveCurrent(false);
        g_resumeCurPath.clear();
        bool idle = WaitResumeIdle(5000);
        {
            std::lock_guard<std::mutex> lk(g_resumeQMtx);
            g_resumeStop = true;
        }
        g_resumeQCv.notify_all();
        if (g_resumeThread.joinable()) { if (idle) g_resumeThread.join(); else g_resumeThread.detach(); }
        // Make blocked stream reads fail fast, then shut mpv down.
        g_streamConn.disconnect();
        g_browseConn.disconnect();
        if (g_mpvh) {
            mpv_handle* m = g_mpvh;
            g_mpvh = nullptr;
            g_mpv.terminate_destroy(m);
        }
        DestroyWindow(h);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
static int AskpassMain() {
    // ssh.exe runs us as SSH_ASKPASS: print the password and exit.
    std::vector<wchar_t> pw(32768, 0);
    DWORD n = GetEnvironmentVariableW(L"SSHWINPLAYER_PW", pw.data(), (DWORD)pw.size());
    std::string s = WideToUtf8(std::wstring(pw.data(), n)) + "\n";
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), (DWORD)s.size(), &w, nullptr);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    wchar_t flag[8];
    if (GetEnvironmentVariableW(L"SSHWINPLAYER_ASKPASS", flag, 8) && flag[0] == L'1') return AskpassMain();

    g_inst = inst;
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    std::wstring dir = AppDataDir();
    g_iniPath = dir + L"\\settings.ini";
    g_logFile = _wfopen((dir + L"\\log.txt").c_str(), L"w");

    // Load libmpv from the executable's folder.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exeDir = exe;
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));
    SetDllDirectoryW(exeDir.c_str());
    if (!g_mpv.load((exeDir + L"\\libmpv-2.dll").c_str()) && !g_mpv.load(L"libmpv-2.dll")) {
        MessageBoxW(nullptr,
                    L"libmpv-2.dll could not be loaded.\n\nPut libmpv-2.dll (64-bit) next to SshWinPlayer.exe.",
                    kAppName, MB_ICONERROR);
        return 1;
    }

    HDC dc = GetDC(nullptr);
    g_scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 13 / 10;
    wcscpy(lf.lfFaceName, L"Segoe UI Symbol");
    g_symFont = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 2;
    g_bigFont = CreateFontIndirectW(&lf);

    g_listW = S(IniGetInt(L"main", L"listWidth", 340));
    g_showList = IniGetInt(L"main", L"showList", 1) != 0;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = S(1200), hgt = S(760);
    bool maximized = false;
    {
        std::wstring win = IniGet(L"main", L"win");
        int l, t, r, b, m;
        if (swscanf(win.c_str(), L"%d,%d,%d,%d,%d", &l, &t, &r, &b, &m) == 5 && r - l > 200 && b - t > 200) {
            RECT rr = {l, t, r, b};
            if (MonitorFromRect(&rr, MONITOR_DEFAULTTONULL)) { x = l; y = t; w = r - l; hgt = b - t; }
            maximized = m != 0;
        }
    }
    HWND h = CreateWindowExW(0, kMainClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, hgt, nullptr,
                             nullptr, inst, nullptr);
    if (!h) return 1;

    std::string err;
    if (!InitMpv(err)) {
        MessageBoxW(h, (L"Could not start the player: " + Utf8ToWide(err)).c_str(), kAppName, MB_ICONERROR);
        return 1;
    }

    ShowWindow(h, maximized ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(h);
    Layout();
    SetConnStatus();
    NavigateLocal(IniGet(L"main", L"localPath"));
    if (!g_entries.size()) NavigateLocal(L"");
    UpdateUiFromState();

    // Files passed on the command line (e.g. "Open with").
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> files;
    for (int i = 1; argv && i < argc; ++i) files.push_back(argv[i]);
    if (argv) LocalFree(argv);
    if (!files.empty()) PlayLocalFiles(files);
    SetFocus(g_main);

    g_resumeThread = std::thread(ResumeWorker);
    // Broken connections (either one) -> "SSH connection reset" dialog on the UI thread.
    auto lost = [](const std::string& e) { PostMessageW(g_main, WM_SSH_LOST, 0, (LPARAM) new std::string(e)); };
    g_browseConn.setLostCallback(lost);
    g_streamConn.setLostCallback(lost);
    SetTimer(g_main, kResumeTimer, kResumeTimerMs, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (PreTranslate(m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_logFile) fclose(g_logFile);
    return 0;
}
