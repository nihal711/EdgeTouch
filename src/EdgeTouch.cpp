// EdgeTouch
//
// Fixes the Windows cursor showing up on your crosshair in CS2 (or another fullscreen game)
// after you use a second touchscreen such as the Corsair Xeneon Edge.
//
// In-game, CS2 locks the mouse to the middle of the screen and hides the cursor. Touching an
// app on the Edge makes that app set the normal arrow. Windows only re-checks which window's
// cursor to show when the mouse moves over a window, and with the mouse locked in place that
// never happens, so the arrow stays on screen until something forces a refresh (which is why
// opening and closing the game menu clears it).
//
// EdgeTouch watches for that state and forces the refresh: it lifts the cursor clip for an
// instant, moves the cursor one pixel and back so Windows re-checks the game window, and puts
// the clip back exactly as it was. It installs no input hooks, sends no keystrokes or clicks,
// and never opens or touches the game's process.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#include "resource.h"

#ifndef CURSOR_SUPPRESSED
#define CURSOR_SUPPRESSED 0x00000002
#endif

namespace {

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
const wchar_t kVersion[] = WIDEN(EDGETOUCH_VERSION_STR);
const wchar_t kAppName[] = L"EdgeTouch";
const wchar_t kSection[] = L"EdgeTouch";
const wchar_t kMainClass[] = L"EdgeTouch.Main";
const wchar_t kSettingsClass[] = L"EdgeTouch.Settings";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 2;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT_PTR kRefreshTimer = 2;

// Watcher timing.
constexpr UINT kPollIdleMs = 50;
constexpr UINT kPollActiveMs = 10;
constexpr ULONGLONG kResendMs = 80;        // gap between nudges while the cursor is visible
constexpr ULONGLONG kSlowResendMs = 1000;  // gap once the quick nudges have been used up
constexpr unsigned kQuickNudges = 6;       // nudges before deciding the game wants the cursor
constexpr ULONGLONG kSettledMs = 1500;     // cursor hidden this long = fixed, stop watching
constexpr LONG kLockedClipSize = 16;       // a clip this small means the game has the mouse

// Menu and control IDs.
enum : UINT {
    ID_TRAY_STATUS = 100,
    ID_TRAY_SETTINGS,
    ID_TRAY_ENABLED,
    ID_TRAY_EXIT,

    IDC_GRP_GENERAL = 200,
    IDC_ENABLED,
    IDC_STARTUP,
    IDC_GRP_GAMES,
    IDC_FULLSCREEN,
    IDC_GAMES_LABEL,
    IDC_GAMES,
    IDC_ADD,
    IDC_REMOVE,
    IDC_GRP_TROUBLE,
    IDC_DEBUGLOG,
    IDC_OPENLOG,
};

const wchar_t kDefaultIni[] =
    L"; EdgeTouch settings. Normally changed from the Settings window (click the tray icon).\r\n"
    L"; Changes made here by hand are picked up the next time EdgeTouch starts.\r\n"
    L"[EdgeTouch]\r\n"
    L"Enabled=1\r\n"
    L"FullscreenIsGame=1\r\n"
    L"Games=cs2.exe\r\n"
    L"DebugLog=0\r\n";

struct Config {
    bool enabled = true;
    bool fullscreenIsGame = true;
    bool debugLog = false;
    std::vector<std::wstring> games = {L"cs2.exe"};
};

HINSTANCE g_inst = nullptr;
HWND g_wnd = nullptr;
HWND g_settings = nullptr;
Config g_cfg;
std::wstring g_dir, g_iniPath, g_logPath;
UINT g_taskbarCreatedMsg = 0;
HICON g_trayIcon = nullptr;
int g_trayIconState = -1;

// Watcher state.
UINT g_pollMs = 0;
bool g_armed = false;
ULONGLONG g_lastSend = 0;
ULONGLONG g_hiddenSince = 0;
unsigned g_sendsThisRound = 0;
unsigned g_fixesTotal = 0;
HWND g_cachedFg = nullptr;
bool g_cachedFgIsGame = false;
ULONGLONG g_cachedAt = 0;

// ---------------------------------------------------------------------------------------------
// Helpers

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring Trim(const std::wstring& s) {
    const auto b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    const auto e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

std::wstring FileNameOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ClassOf(HWND h) {
    wchar_t buf[128];
    const int n = GetClassNameW(h, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n > 0 ? n : 0);
}

bool IsShellWindow(HWND h) {
    static const wchar_t* const kShell[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
        L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
    };
    const std::wstring c = ClassOf(h);
    for (const wchar_t* k : kShell)
        if (c == k) return true;
    return false;
}

std::wstring ProcessNameOfWindow(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return L"";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    std::wstring name;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ARRAYSIZE(buf);
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) name = Lower(FileNameOf(std::wstring(buf, n)));
    CloseHandle(p);
    return name;
}

void Log(const wchar_t* fmt, ...) {
    if (!g_cfg.debugLog) return;
    wchar_t msg[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(msg, ARRAYSIZE(msg) - 1, fmt, args);
    va_end(args);
    msg[ARRAYSIZE(msg) - 1] = 0;

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[600];
    _snwprintf(line, ARRAYSIZE(line) - 1, L"%02u:%02u:%02u.%03u  %s\r\n", t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds, msg);
    line[ARRAYSIZE(line) - 1] = 0;

    char utf8[1800];
    const int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (len <= 1) return;
    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, utf8, static_cast<DWORD>(len - 1), &written, nullptr);
    CloseHandle(f);
}

// ---------------------------------------------------------------------------------------------
// Settings storage

void WriteDefaultIni() {
    HANDLE f = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    const wchar_t bom = 0xFEFF;  // UTF-16, so the profile APIs keep the file Unicode
    WriteFile(f, &bom, sizeof(bom), &w, nullptr);
    WriteFile(f, kDefaultIni, static_cast<DWORD>(wcslen(kDefaultIni) * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
}

bool IniBool(const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(kSection, key, def ? 1 : 0, g_iniPath.c_str()) != 0;
}

void IniPut(const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(kSection, key, value.c_str(), g_iniPath.c_str());
}

std::vector<std::wstring> ParseGames(const std::wstring& all) {
    std::vector<std::wstring> games;
    size_t start = 0;
    while (start <= all.size()) {
        const size_t comma = all.find(L',', start);
        const size_t len = comma == std::wstring::npos ? std::wstring::npos : comma - start;
        const std::wstring item = Lower(Trim(all.substr(start, len)));
        if (!item.empty() && std::find(games.begin(), games.end(), item) == games.end()) games.push_back(item);
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return games;
}

void LoadConfig() {
    const Config d;
    g_cfg.enabled = IniBool(L"Enabled", d.enabled);
    g_cfg.fullscreenIsGame = IniBool(L"FullscreenIsGame", d.fullscreenIsGame);
    g_cfg.debugLog = IniBool(L"DebugLog", d.debugLog);
    wchar_t buf[4096];
    GetPrivateProfileStringW(kSection, L"Games", L"cs2.exe", buf, ARRAYSIZE(buf), g_iniPath.c_str());
    g_cfg.games = ParseGames(buf);
    g_cachedFg = nullptr;
}

void SaveConfig() {
    IniPut(L"Enabled", g_cfg.enabled ? L"1" : L"0");
    IniPut(L"FullscreenIsGame", g_cfg.fullscreenIsGame ? L"1" : L"0");
    IniPut(L"DebugLog", g_cfg.debugLog ? L"1" : L"0");
    std::wstring games;
    for (const auto& g : g_cfg.games) games += (games.empty() ? L"" : L", ") + g;
    IniPut(L"Games", games);
}

bool StartupEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    const LONG r = RegQueryValueExW(k, kAppName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

void SetStartup(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return;
    if (on) {
        const std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------------------------
// Game detection

bool IsFullscreen(HWND h) {
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;  // normal or maximized window
    RECT wr;
    if (!GetWindowRect(h, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    const RECT& r = mi.rcMonitor;
    return wr.left <= r.left && wr.top <= r.top && wr.right >= r.right && wr.bottom >= r.bottom;
}

bool IsGameWindow(HWND h) {
    if (!h || !IsWindow(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() || IsShellWindow(h)) return false;
    const std::wstring name = ProcessNameOfWindow(h);
    if (!name.empty() && std::find(g_cfg.games.begin(), g_cfg.games.end(), name) != g_cfg.games.end())
        return true;
    return g_cfg.fullscreenIsGame && IsFullscreen(h);
}

bool ForegroundIsGame(HWND fg, ULONGLONG now) {
    // Cache per foreground window, rechecking now and then in case the game switches between
    // windowed and fullscreen.
    if (fg != g_cachedFg || now - g_cachedAt > 2000) {
        const bool changed = fg != g_cachedFg;
        const bool wasGame = g_cachedFgIsGame;
        g_cachedFg = fg;
        g_cachedAt = now;
        g_cachedFgIsGame = IsGameWindow(fg);
        if (fg && (changed || wasGame != g_cachedFgIsGame)) {
            Log(L"foreground: %s [%s] game=%d fullscreen=%d", ProcessNameOfWindow(fg).c_str(),
                ClassOf(fg).c_str(), g_cachedFgIsGame ? 1 : 0, IsFullscreen(fg) ? 1 : 0);
        }
    }
    return g_cachedFgIsGame;
}

// ---------------------------------------------------------------------------------------------
// The fix

void SetPollRate(UINT ms) {
    if (ms == g_pollMs) return;
    g_pollMs = ms;
    SetTimer(g_wnd, kPollTimer, ms, nullptr);
}

bool IsLockedClip(const RECT& clip) {
    return clip.right - clip.left <= kLockedClipSize && clip.bottom - clip.top <= kLockedClipSize;
}

void Arm(const wchar_t* why, const CURSORINFO& ci, const RECT& clip) {
    if (g_armed) return;
    Log(L"watching: %s (cursor %ld,%ld flags=0x%lx shape=%p clip=%ld,%ld-%ld,%ld)", why, ci.ptScreenPos.x,
        ci.ptScreenPos.y, static_cast<unsigned long>(ci.flags), static_cast<void*>(ci.hCursor), clip.left,
        clip.top, clip.right, clip.bottom);
    g_armed = true;
    g_sendsThisRound = 0;
    g_hiddenSince = 0;
    SetPollRate(kPollActiveMs);
}

void Disarm(const wchar_t* why) {
    if (g_armed) Log(L"done: %s (%u nudge(s))", why, g_sendsThisRound);
    g_armed = false;
    g_hiddenSince = 0;
    g_sendsThisRound = 0;
    SetPollRate(kPollIdleMs);
}

// Make Windows re-check which window's cursor should be shown.
//
// Windows shows the cursor of the window it last hit-tested under the mouse. While the game
// holds the mouse still, that check never happens, so the arrow set by the Edge app stays.
// Moving the cursor one pixel and back triggers the check; the game then gets WM_SETCURSOR and
// applies its own hidden cursor. The game's clip is lifted for that instant (the clip is a
// desktop-wide setting) and restored exactly. SetCursorPos produces no raw input, so aim is
// unaffected.
void NudgeGame(HWND game, POINT pt, const RECT& gameRect) {
    RECT clip{};
    const bool haveClip = GetClipCursor(&clip) != FALSE;
    ClipCursor(nullptr);
    const LONG dx = (pt.x + 1 < gameRect.right) ? 1 : -1;
    SetCursorPos(pt.x + dx, pt.y);
    SetCursorPos(pt.x, pt.y);
    if (haveClip) ClipCursor(&clip);

    HWND target = WindowFromPoint(pt);
    if (!target || GetAncestor(target, GA_ROOT) != game) target = game;
    PostMessageW(target, WM_SETCURSOR, reinterpret_cast<WPARAM>(target), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void Poll() {
    static int lastVisible = -1;
    const ULONGLONG now = GetTickCount64();
    if (!g_cfg.enabled) {
        Disarm(L"disabled");
        return;
    }

    HWND fg = GetForegroundWindow();
    if (!ForegroundIsGame(fg, now)) {
        if (g_armed) Disarm(L"game not in front");
        lastVisible = -1;
        return;
    }

    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return;
    RECT wr, clip{};
    if (!GetWindowRect(fg, &wr)) return;
    GetClipCursor(&clip);

    const bool inGame = PtInRect(&wr, ci.ptScreenPos) != FALSE;
    const bool visible = (ci.flags & CURSOR_SHOWING) && ci.hCursor != nullptr;
    const bool locked = IsLockedClip(clip);

    if (visible != (lastVisible == 1)) {
        Log(L"cursor %s (at %ld,%ld in_game=%d flags=0x%lx shape=%p clip=%ld,%ld-%ld,%ld)",
            visible ? L"visible" : L"hidden", ci.ptScreenPos.x, ci.ptScreenPos.y, inGame ? 1 : 0,
            static_cast<unsigned long>(ci.flags), static_cast<void*>(ci.hCursor), clip.left, clip.top, clip.right,
            clip.bottom);
        lastVisible = visible ? 1 : 0;
    }

    if (!inGame) {
        // While playing, the cursor only leaves the game window if something else moved it.
        Arm(L"cursor left the game window", ci, clip);
        g_hiddenSince = 0;
        return;
    }

    // A visible cursor while the game holds the mouse in a tiny area is the bug itself.
    if (!g_armed) {
        if (visible && locked) Arm(L"cursor visible while the game has the mouse locked", ci, clip);
        else return;
    }

    if (visible) {
        g_hiddenSince = 0;
        ULONGLONG gap = kResendMs;
        if (g_sendsThisRound >= kQuickNudges) {
            // Still visible. If the mouse isn't locked, the game wants its cursor shown (a menu),
            // so leave it alone. If it is locked, keep trying, slowly.
            if (!locked) {
                Disarm(L"game is showing its own cursor");
                return;
            }
            gap = kSlowResendMs;
        }
        if (now - g_lastSend >= gap) {
            NudgeGame(fg, ci.ptScreenPos, wr);
            g_lastSend = now;
            if (g_sendsThisRound == 0) g_fixesTotal++;
            ++g_sendsThisRound;
            if (g_sendsThisRound <= kQuickNudges || g_sendsThisRound % 10 == 0) Log(L"nudge #%u", g_sendsThisRound);
        }
    } else if (!(ci.flags & CURSOR_SUPPRESSED)) {
        if (!g_hiddenSince) g_hiddenSince = now;
        else if (now - g_hiddenSince >= kSettledMs) Disarm(L"cursor hidden");
    }
}

// ---------------------------------------------------------------------------------------------
// Tray icon

HICON LoadAppIcon(UINT id, int size) {
    return static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

NOTIFYICONDATAW BaseNid() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    return nid;
}

void SetTip(NOTIFYICONDATAW& nid) {
    const wchar_t* tip = g_cfg.enabled ? L"EdgeTouch" : L"EdgeTouch (paused)";
    wcsncpy(nid.szTip, tip, ARRAYSIZE(nid.szTip) - 1);
}

void RefreshTrayIcon() {
    const int state = g_cfg.enabled ? 1 : 0;
    if (state == g_trayIconState && g_trayIcon) return;
    HICON old = g_trayIcon;
    HICON icon = nullptr;
    if (FAILED(LoadIconMetric(g_inst, MAKEINTRESOURCEW(state ? IDI_APP : IDI_OFF), LIM_SMALL, &icon)) || !icon)
        icon = LoadAppIcon(state ? IDI_APP : IDI_OFF, GetSystemMetrics(SM_CXSMICON));
    g_trayIcon = icon;
    g_trayIconState = state;
    if (old) DestroyIcon(old);
}

void AddTray() {
    RefreshTrayIcon();
    NOTIFYICONDATAW nid = BaseNid();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_trayIcon;
    SetTip(nid);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void UpdateTray() {
    RefreshTrayIcon();
    NOTIFYICONDATAW nid = BaseNid();
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = g_trayIcon;
    SetTip(nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowFirstRunNotice() {
    NOTIFYICONDATAW nid = BaseNid();
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(nid.szInfoTitle, L"EdgeTouch is running", ARRAYSIZE(nid.szInfoTitle) - 1);
    wcsncpy(nid.szInfo, L"It sits here in the tray. Click the icon to open settings.", ARRAYSIZE(nid.szInfo) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowSettings();
void SyncSettingsEnabled();

void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    wchar_t status[128];
    if (!g_cfg.enabled) wcsncpy(status, L"Paused", ARRAYSIZE(status));
    else if (g_fixesTotal == 0) wcsncpy(status, L"Running", ARRAYSIZE(status));
    else _snwprintf(status, ARRAYSIZE(status) - 1, L"Running, cursor cleared %u time%s", g_fixesTotal,
                    g_fixesTotal == 1 ? L"" : L"s");
    status[ARRAYSIZE(status) - 1] = 0;

    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_TRAY_STATUS, status);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_SETTINGS, L"Settings...");
    AppendMenuW(m, MF_STRING | (g_cfg.enabled ? MF_CHECKED : 0), ID_TRAY_ENABLED, L"Enabled");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetMenuDefaultItem(m, ID_TRAY_SETTINGS, FALSE);

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------------------------------------
// Settings window

struct ControlDef {
    UINT id;
    const wchar_t* cls;
    const wchar_t* text;
    DWORD style;
    DWORD exStyle;
    int x, y, w, h;  // in 96-DPI units
};

constexpr int kSettingsW = 460;
constexpr int kSettingsH = 500;
constexpr int kHeaderH = 64;

const ControlDef kControls[] = {
    {IDC_GRP_GENERAL, L"BUTTON", L"General", BS_GROUPBOX, 0, 16, 72, 428, 80},
    {IDC_ENABLED, L"BUTTON", L"&Enable EdgeTouch", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 0, 30, 96, 400, 20},
    {IDC_STARTUP, L"BUTTON", L"&Start when I sign in to Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 30, 120, 400, 20},

    {IDC_GRP_GAMES, L"BUTTON", L"Games", BS_GROUPBOX, 0, 16, 164, 428, 208},
    {IDC_FULLSCREEN, L"BUTTON", L"Treat any &fullscreen app as a game", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 0,
     30, 188, 400, 20},
    {IDC_GAMES_LABEL, L"STATIC", L"Also watch these programs, even when they run in a window:", SS_LEFT, 0, 30, 216,
     400, 18},
    {IDC_GAMES, L"LISTBOX", L"",
     LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_SORT | WS_VSCROLL | WS_TABSTOP | WS_GROUP, WS_EX_CLIENTEDGE, 30, 238,
     290, 120},
    {IDC_ADD, L"BUTTON", L"&Add...", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 332, 238, 98, 28},
    {IDC_REMOVE, L"BUTTON", L"&Remove", BS_PUSHBUTTON | WS_TABSTOP, 0, 332, 272, 98, 28},

    {IDC_GRP_TROUBLE, L"BUTTON", L"Troubleshooting", BS_GROUPBOX, 0, 16, 384, 428, 60},
    {IDC_DEBUGLOG, L"BUTTON", L"Write a &debug log", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 0, 30, 410, 240, 20},
    {IDC_OPENLOG, L"BUTTON", L"&Open log folder", BS_PUSHBUTTON | WS_TABSTOP, 0, 290, 405, 140, 28},

    {IDOK, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 262, 458, 88, 28},
    {IDCANCEL, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, 356, 458, 88, 28},
};

UINT g_dpi = 96;
HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
HICON g_headerIcon = nullptr;

int Scale(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }

void CreateFonts() {
    if (g_font) DeleteObject(g_font);
    if (g_titleFont) DeleteObject(g_titleFont);
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(9, static_cast<int>(g_dpi), 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, L"Segoe UI", LF_FACESIZE - 1);
    g_font = CreateFontIndirectW(&lf);
    lf.lfHeight = -MulDiv(15, static_cast<int>(g_dpi), 72);
    lf.lfWeight = FW_SEMIBOLD;
    g_titleFont = CreateFontIndirectW(&lf);

    if (g_headerIcon) DestroyIcon(g_headerIcon);
    g_headerIcon = LoadAppIcon(IDI_APP, Scale(40));
}

void LayoutSettings(HWND w) {
    for (const auto& c : kControls) {
        HWND h = GetDlgItem(w, static_cast<int>(c.id));
        if (!h) continue;
        SetWindowPos(h, nullptr, Scale(c.x), Scale(c.y), Scale(c.w), Scale(c.h), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    }
    InvalidateRect(w, nullptr, TRUE);
}

void SetCheck(HWND w, UINT id, bool on) { CheckDlgButton(w, static_cast<int>(id), on ? BST_CHECKED : BST_UNCHECKED); }
bool GetCheck(HWND w, UINT id) { return IsDlgButtonChecked(w, static_cast<int>(id)) == BST_CHECKED; }

void UpdateSettingsButtons(HWND w) {
    HWND list = GetDlgItem(w, IDC_GAMES);
    const bool hasSel = SendMessageW(list, LB_GETCURSEL, 0, 0) != LB_ERR;
    EnableWindow(GetDlgItem(w, IDC_REMOVE), hasSel);
}

void FillSettings(HWND w) {
    SetCheck(w, IDC_ENABLED, g_cfg.enabled);
    SetCheck(w, IDC_STARTUP, StartupEnabled());
    SetCheck(w, IDC_FULLSCREEN, g_cfg.fullscreenIsGame);
    SetCheck(w, IDC_DEBUGLOG, g_cfg.debugLog);
    HWND list = GetDlgItem(w, IDC_GAMES);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& g : g_cfg.games) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g.c_str()));
    UpdateSettingsButtons(w);
}

void SyncSettingsEnabled() {
    if (g_settings) SetCheck(g_settings, IDC_ENABLED, g_cfg.enabled);
}

void AddGame(HWND w) {
    wchar_t path[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = w;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAYSIZE(path);
    ofn.lpstrTitle = L"Choose a game";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    const std::wstring name = Lower(FileNameOf(path));
    HWND list = GetDlgItem(w, IDC_GAMES);
    LRESULT idx = SendMessageW(list, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name.c_str()));
    if (idx == LB_ERR) idx = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(idx), 0);
    UpdateSettingsButtons(w);
}

void RemoveGame(HWND w) {
    HWND list = GetDlgItem(w, IDC_GAMES);
    const LRESULT sel = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    SendMessageW(list, LB_DELETESTRING, static_cast<WPARAM>(sel), 0);
    const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
    if (count > 0) SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(std::min<LRESULT>(sel, count - 1)), 0);
    UpdateSettingsButtons(w);
}

void SaveSettings(HWND w) {
    g_cfg.enabled = GetCheck(w, IDC_ENABLED);
    g_cfg.fullscreenIsGame = GetCheck(w, IDC_FULLSCREEN);
    g_cfg.debugLog = GetCheck(w, IDC_DEBUGLOG);

    g_cfg.games.clear();
    HWND list = GetDlgItem(w, IDC_GAMES);
    const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; ++i) {
        const LRESULT len = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(i), 0);
        if (len <= 0) continue;
        std::wstring item(static_cast<size_t>(len) + 1, L'\0');
        SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&item[0]));
        item.resize(static_cast<size_t>(len));
        g_cfg.games.push_back(item);
    }

    SaveConfig();
    const bool wantStartup = GetCheck(w, IDC_STARTUP);
    if (wantStartup != StartupEnabled()) SetStartup(wantStartup);
    g_cachedFg = nullptr;
    Log(L"settings saved: enabled=%d fullscreen=%d games=%u", g_cfg.enabled ? 1 : 0, g_cfg.fullscreenIsGame ? 1 : 0,
        static_cast<unsigned>(g_cfg.games.size()));
    UpdateTray();
}

void OpenLogFolder() {
    if (GetFileAttributesW(g_logPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring args = L"/select,\"" + g_logPath + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", g_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void PaintSettingsHeader(HDC dc) {
    const int pad = Scale(16);
    if (g_headerIcon) DrawIconEx(dc, pad, Scale(14), g_headerIcon, Scale(40), Scale(40), 0, nullptr, DI_NORMAL);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g_titleFont);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT r{Scale(68), Scale(12), Scale(kSettingsW) - pad, Scale(40)};
    DrawTextW(dc, kAppName, -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    SelectObject(dc, g_font);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    wchar_t sub[160];
    if (!g_cfg.enabled)
        _snwprintf(sub, ARRAYSIZE(sub) - 1, L"Version %s  \x00B7  Paused", kVersion);
    else if (g_fixesTotal == 0)
        _snwprintf(sub, ARRAYSIZE(sub) - 1, L"Version %s  \x00B7  Running", kVersion);
    else
        _snwprintf(sub, ARRAYSIZE(sub) - 1, L"Version %s  \x00B7  Running  \x00B7  Cursor cleared %u time%s this session",
                   kVersion, g_fixesTotal, g_fixesTotal == 1 ? L"" : L"s");
    sub[ARRAYSIZE(sub) - 1] = 0;
    RECT s{Scale(68), Scale(38), Scale(kSettingsW) - pad, Scale(58)};
    DrawTextW(dc, sub, -1, &s, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);

    // Divider under the header.
    RECT line{pad, Scale(kHeaderH), Scale(kSettingsW) - pad, Scale(kHeaderH) + 1};
    FillRect(dc, &line, GetSysColorBrush(COLOR_3DSHADOW));
}

LRESULT CALLBACK SettingsProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_dpi = GetDpiForWindow(w);
            CreateFonts();
            for (const auto& c : kControls) {
                CreateWindowExW(c.exStyle, c.cls, c.text, WS_CHILD | WS_VISIBLE | c.style, 0, 0, 0, 0, w,
                                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(c.id)), g_inst, nullptr);
            }
            LayoutSettings(w);
            FillSettings(w);
            SetTimer(w, kRefreshTimer, 1000, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (wp == kRefreshTimer) {
                RECT header{0, 0, Scale(kSettingsW), Scale(kHeaderH)};
                InvalidateRect(w, &header, TRUE);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(w, &ps);
            PaintSettingsHeader(dc);
            EndPaint(w, &ps);
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            CreateFonts();
            LayoutSettings(w);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                    SaveSettings(w);
                    DestroyWindow(w);
                    return 0;
                case IDCANCEL:
                    DestroyWindow(w);
                    return 0;
                case IDC_ADD:
                    AddGame(w);
                    return 0;
                case IDC_REMOVE:
                    RemoveGame(w);
                    return 0;
                case IDC_OPENLOG:
                    OpenLogFolder();
                    return 0;
                case IDC_GAMES:
                    if (HIWORD(wp) == LBN_SELCHANGE) UpdateSettingsButtons(w);
                    return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            KillTimer(w, kRefreshTimer);
            g_settings = nullptr;
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void ShowSettings() {
    if (g_settings) {
        if (IsIconic(g_settings)) ShowWindow(g_settings, SW_RESTORE);
        SetForegroundWindow(g_settings);
        return;
    }

    // Open on the monitor the mouse is on, centred in its work area.
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& work = mi.rcWork;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_CONTROLPARENT;
    g_settings = CreateWindowExW(exStyle, kSettingsClass, L"EdgeTouch Settings", style, work.left + 50,
                                 work.top + 50, 100, 100, nullptr, nullptr, g_inst, nullptr);
    if (!g_settings) return;

    RECT r{0, 0, Scale(kSettingsW), Scale(kSettingsH)};
    AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, g_dpi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const int x = work.left + ((work.right - work.left) - ww) / 2;
    const int y = work.top + ((work.bottom - work.top) - wh) / 2;
    SetWindowPos(g_settings, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_settings, SW_SHOWNORMAL);
    SetForegroundWindow(g_settings);
    SetFocus(GetDlgItem(g_settings, IDC_ENABLED));
}

// ---------------------------------------------------------------------------------------------
// Main window (hidden; owns the tray icon and the poll timer)

void Shutdown() {
    static bool done = false;
    if (done) return;
    done = true;
    KillTimer(g_wnd, kPollTimer);
    if (g_settings) DestroyWindow(g_settings);
    NOTIFYICONDATAW nid = BaseNid();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = nullptr;
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreatedMsg && msg == g_taskbarCreatedMsg) {  // Explorer restarted
        g_trayIconState = -1;
        AddTray();
        return 0;
    }
    switch (msg) {
        case WM_TIMER:
            if (wp == kPollTimer) Poll();
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                    ShowSettings();
                    break;
                case WM_RBUTTONUP:
                    ShowTrayMenu();
                    break;
            }
            return 0;
        case WM_APP_SHOW_SETTINGS:
            ShowSettings();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_TRAY_SETTINGS:
                    ShowSettings();
                    break;
                case ID_TRAY_ENABLED:
                    g_cfg.enabled = !g_cfg.enabled;
                    IniPut(L"Enabled", g_cfg.enabled ? L"1" : L"0");
                    Log(L"enabled = %d", g_cfg.enabled ? 1 : 0);
                    SyncSettingsEnabled();
                    UpdateTray();
                    break;
                case ID_TRAY_EXIT:
                    DestroyWindow(g_wnd);
                    break;
            }
            return 0;
        case WM_ENDSESSION:
            if (wp) Shutdown();
            return 0;
        case WM_DESTROY:
            Shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void InitPaths() {
    wchar_t appData[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, ARRAYSIZE(appData));
    if (n > 0 && n < ARRAYSIZE(appData)) {
        g_dir = std::wstring(appData) + L"\\EdgeTouch";
    } else {
        const std::wstring exe = ExePath();
        g_dir = exe.substr(0, exe.find_last_of(L"\\/"));
    }
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_iniPath = g_dir + L"\\EdgeTouch.ini";
    g_logPath = g_dir + L"\\EdgeTouch.log";
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    g_inst = inst;

    // Starting it again just opens the settings of the copy that's already running.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\EdgeTouch.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, WM_APP_SHOW_SETTINGS, 0, 0);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    InitPaths();
    const bool firstRun = GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (firstRun) WriteDefaultIni();
    LoadConfig();

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.hIcon = LoadAppIcon(IDI_APP, GetSystemMetrics(SM_CXICON));
    wc.hIconSm = LoadAppIcon(IDI_APP, GetSystemMetrics(SM_CXSMICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = kSettingsClass;
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    RegisterClassExW(&wc);

    // A hidden top-level window rather than a message-only one, so it gets the TaskbarCreated
    // broadcast when Explorer restarts.
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                            nullptr);
    if (!g_wnd) return 1;

    AddTray();
    if (firstRun) ShowFirstRunNotice();
    SetPollRate(kPollIdleMs);
    Log(L"EdgeTouch %s started", kVersion);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_settings && IsDialogMessageW(g_settings, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shutdown();
    if (mutex) CloseHandle(mutex);
    return 0;
}
