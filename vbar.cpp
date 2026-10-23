// vbar.cpp - text-only vertical taskbar for Windows 10/11
// Old-school bevel buttons: black outline, white corner highlight, dark grey face, light grey text.
// Running windows at the top, pinned apps (yours + Windows Start pins) at the bottom, clock last.
//
// Build (MSVC, x64 Native Tools prompt):
//   cl /O2 /EHsc /DUNICODE /D_UNICODE vbar.cpp /link /SUBSYSTEM:WINDOWS
// Build (MinGW-w64):
//   g++ -O2 -municode -mwindows -static vbar.cpp -o vbar.exe -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -luuid -lmsimg32 -ladvapi32
//
// First run writes vbar.ini next to the exe. Edit it, then right-click the clock > Reload.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <map>
#include <memory>
#include <cstdint>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#ifdef _MSC_VER
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

#define VBAR_VERSION L"1.4"

#define WM_APPBAR    (WM_APP + 1)
#define WM_STARTPINS (WM_APP + 2)
#define TIMER_CLOCK 1
#define TIMER_SCAN 2
#define TIMER_HOVER 3
#define TIMER_METER 4
#define TIMER_BLINK 5
#define TIMER_CAL 6
#define IDM_RELOAD 100
#define IDM_EDIT 101
#define IDM_EXIT 102
#define IDM_SIDE_LEFT 110
#define IDM_SIDE_RIGHT 111
#define IDM_CLOCK24 112
#define IDM_AUTOSTART 113
#define IDM_MON_PRIMARY 119
#define IDM_MON_BASE 120
#define IDM_T_SWITCH   200
#define IDM_T_RESTORE  201
#define IDM_T_MINIMIZE 202
#define IDM_T_MAXIMIZE 203
#define IDM_T_CLOSE    204
#define IDM_T_LOCATION 205
#define IDM_T_KILL     206
#define IDM_T_NEW      207
#define IDM_P_OPEN     300
#define IDM_P_ADMIN    301
#define IDM_P_LOCATION 302

enum Kind { K_PIN = 0, K_TASK = 1, K_CLOCK = 2, K_START = 3, K_NONE = -1 };

// A 2-bit icon: every pixel is transparent or one of three grey shades, stored ready to AlphaBlend.
struct Icon2 { HBITMAP bmp = nullptr; int size = 0; ~Icon2() { if (bmp) DeleteObject(bmp); } };
typedef std::shared_ptr<Icon2> IconPtr;

struct Pin  { std::wstring name, path, args, dir, aumid; };
struct Task { HWND hwnd; std::wstring title; bool flash; bool iconic; bool visible;
              DWORD pid = 0; std::wstring exe; float level = 0;
              IconPtr icon; int iconTries = 0; bool iconFinal = false; DWORD iconNext = 0; };
struct Row  { RECT r; int kind; int idx; int clipTop; int clipBottom; };
struct Item { int kind; int idx; std::vector<std::wstring> lines; int h; };

struct Config {
    int width = 120, gap = 6, spacing = 2, pad = 4, vpad = 2, lineGap = 0, maxLines = 3;
    int highlight = 4, fontSize = 12, fontWeight = 400, pinsMax = 60, hoverDelay = 0, borderMode = 1;
    std::wstring font = L"Consolas", fontFile, pinDir;
    bool aa = false, clock = true, hideTaskbar = false, ellipsis = true, startPins = true, italic = false;
    bool bold = false, hoverRestore = false, hoverFocus = true, meter = true, rightSide = false;
    int monitor = 0;   // 0 = primary, otherwise Windows display number
    int clockFontSize = 0, clockLines = 2, clockBold = -1, startHeight = 0;   // 0 / -1 = follow the main font
    bool startButton = true, clock24 = false;
    int weekStart = 0;   // 0 = Sunday, 1 = Monday
    int calTimeout = 5;  // seconds the calendar stays up once the mouse leaves it
    COLORREF startColor = RGB(216, 216, 216);
    bool startColorSet = false;
    int meterH = 2, meterFall = 4, meterDb = 48, meterFps = 30;
    COLORREF bg = RGB(0, 0, 0), face = RGB(64, 64, 64), text = RGB(216, 216, 216),
             hi = RGB(255, 255, 255), dim = RGB(136, 136, 136), flash = RGB(110, 110, 110),
             outline = RGB(0, 0, 0), hoverText = RGB(255, 255, 255), activeText = RGB(255, 255, 255),
             activeFace = RGB(64, 64, 64), pinFace = RGB(64, 64, 64), clockFace = RGB(64, 64, 64),
             activeBorder = RGB(255, 255, 255), meterColor = RGB(47, 125, 255);
    COLORREF flashColor = RGB(47, 125, 255);
    bool flashColorSet = false;
    int hoverBright = 28, flashRate = 500, flashCount = 8;
    bool icons = true, iconDither = true;
    int iconSize = 0, iconPad = 3, scrollW = 2;
    COLORREF iconShade[3] = {RGB(0, 0, 0), RGB(140, 140, 140), RGB(232, 232, 232)};
    COLORREF scrollColor = RGB(47, 125, 255);
    bool scrollColorSet = false;
    bool activeFaceSet = false, pinFaceSet = false, clockFaceSet = false;
    std::vector<std::wstring> strip;
    std::vector<Pin> pins;
};

static Config g_cfg;
static HWND g_hwnd = nullptr;
static HFONT g_font = nullptr, g_clockFont = nullptr, g_calBigFont = nullptr;
static HWND g_calHwnd = nullptr;
static int g_calYear = 0, g_calMonth = 0, g_calHover = 0;
static DWORD g_calClosedAt = 0, g_calLastHover = 0;
static WORD g_calLastSecond = 0xFFFF;
static bool g_blinkOn = true;
static int g_blinkTicks = 0;
static std::wstring g_exeDir, g_iniPath, g_loadedFontFile;
static std::vector<Task> g_tasks;
static std::vector<Pin> g_startPins;
static std::vector<Row> g_rows;
static int g_scrollTasks = 0, g_scrollPins = 0, g_lineH = 14;
static int g_tasksTop = 0, g_tasksBottom = 0, g_pinsTop = 0, g_pinsBottom = 0;
static int g_hoverKind = K_NONE, g_hoverIdx = -1;
static bool g_tracking = false;
static UINT g_shellMsg = 0, g_taskbarCreatedMsg = 0;
static HWND g_lastFg = nullptr;
static int g_lastMinute = -1;
static LONG g_startPinsGen = 0;
static std::map<std::wstring, IconPtr> g_pinIcons;
static HDC g_iconDC = nullptr;
static int g_iconS = 0, g_indent = 0;

// ---------- helpers ----------

static std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring DecodeText(const std::string& raw) {
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        std::wstring w((raw.size() - 2) / 2, 0);
        memcpy(&w[0], raw.data() + 2, w.size() * 2);
        return w;
    }
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        return Utf8ToW(raw.substr(3));
    return Utf8ToW(raw);
}

static std::string ReadFileBytes(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<std::wstring> Split(const std::wstring& s, wchar_t d) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(d, start);
        out.push_back(Trim(s.substr(start, p == std::wstring::npos ? std::wstring::npos : p - start)));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    return out;
}

static COLORREF ParseColor(const std::wstring& v, COLORREF def) {
    std::wstring s = v;
    if (!s.empty() && s[0] == L'#') s = s.substr(1);
    if (s.size() != 6) return def;
    unsigned long x = wcstoul(s.c_str(), nullptr, 16);
    return RGB((x >> 16) & 255, (x >> 8) & 255, x & 255);
}

static std::wstring ExpandEnv(const std::wstring& s) {
    if (s.empty()) return s;
    wchar_t buf[2048];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 2048);
    return (n && n <= 2048) ? std::wstring(buf) : s;
}

static std::wstring Resolve(const std::wstring& p) {
    std::wstring e = ExpandEnv(p);
    if (e.size() > 1 && (e[1] == L':' || (e[0] == L'\\' && e[1] == L'\\'))) return e;
    return g_exeDir + L"\\" + e;
}

static std::wstring FileStem(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    std::wstring fn = s == std::wstring::npos ? path : path.substr(s + 1);
    size_t dot = fn.find_last_of(L'.');
    return dot == std::wstring::npos ? fn : fn.substr(0, dot);
}

static void Fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static std::wstring ProcessPath(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(p);
    return out;
}

static void OpenLocation(const std::wstring& path) {
    if (path.empty()) return;
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------- pins ----------

static int PinCount() { return (int)(g_cfg.pins.size() + g_startPins.size()); }
static const Pin& PinAt(int i) {
    return i < (int)g_cfg.pins.size() ? g_cfg.pins[i] : g_startPins[i - g_cfg.pins.size()];
}

static std::vector<Pin> ListShortcutDir(const std::wstring& dir) {
    std::vector<Pin> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fn = fd.cFileName;
        size_t dot = fn.find_last_of(L'.');
        if (dot == std::wstring::npos) continue;
        std::wstring ext = fn.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext != L".lnk" && ext != L".url" && ext != L".exe") continue;
        Pin p;
        p.name = fn.substr(0, dot);
        p.path = dir + L"\\" + fn;
        found.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const Pin& a, const Pin& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return found;
}

// Minimal JSON string reader: s[i] == '"' on entry, returns value, leaves i after the closing quote.
static std::wstring JsonString(const std::wstring& s, size_t& i) {
    std::wstring out;
    ++i;
    while (i < s.size() && s[i] != L'"') {
        wchar_t c = s[i++];
        if (c == L'\\' && i < s.size()) {
            wchar_t e = s[i++];
            switch (e) {
            case L'n': out += L'\n'; break;
            case L't': out += L'\t'; break;
            case L'r': out += L'\r'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'u':
                if (i + 4 <= s.size()) { out += (wchar_t)wcstoul(s.substr(i, 4).c_str(), nullptr, 16); i += 4; }
                break;
            default: out += e; break;     // \\  \"  \/
            }
        } else {
            out += c;
        }
    }
    if (i < s.size()) ++i;
    return out;
}

static std::wstring AppDisplayName(const std::wstring& aumid) {
    std::wstring name;
    std::wstring parse = L"shell:AppsFolder\\" + aumid;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(parse.c_str(), nullptr, IID_IShellItem, (void**)&item)) && item) {
        PWSTR n = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && n) {
            name = n;
            CoTaskMemFree(n);
        }
        item->Release();
    }
    if (name.empty()) {                       // fall back to something readable from the id
        name = aumid;
        size_t bang = name.find(L'!');
        if (bang != std::wstring::npos) name = name.substr(0, bang);
        size_t us = name.find(L'_');
        if (us != std::wstring::npos) name = name.substr(0, us);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot + 1 < name.size()) name = name.substr(dot + 1);
    }
    return name;
}

// Asks Windows for its own Start layout (Export-StartLayout) and turns the pinned list into Pins.
// Falls back to the taskbar's pinned shortcuts folder if that yields nothing.
static std::vector<Pin> LoadStartPins() {
    std::vector<Pin> pins;

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring json = std::wstring(tmp) + L"vbar_startlayout.json";
    DeleteFileW(json.c_str());

    std::wstring quoted;
    for (wchar_t c : json) { quoted += c; if (c == L'\'') quoted += L'\''; }
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
                       L"\"Export-StartLayout -Path '" + quoted + L"'\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 20000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    std::wstring s = DecodeText(ReadFileBytes(json));
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != L'"') { ++i; continue; }
        std::wstring key = JsonString(s, i);
        size_t j = i;
        while (j < s.size() && iswspace(s[j])) ++j;
        if (j >= s.size() || s[j] != L':') continue;
        ++j;
        while (j < s.size() && iswspace(s[j])) ++j;
        if (j >= s.size() || s[j] != L'"') { i = j; continue; }
        i = j;
        std::wstring val = JsonString(s, i);
        if (val.empty()) continue;
        Pin p;
        if (key == L"desktopAppLink") {
            p.path = ExpandEnv(val);
            p.name = FileStem(p.path);
        } else if (key == L"packagedAppId" || key == L"desktopAppId") {
            p.aumid = val;
            p.name = AppDisplayName(val);
        } else {
            continue;
        }
        if (!p.name.empty()) pins.push_back(p);
    }
    DeleteFileW(json.c_str());

    if (pins.empty())
        pins = ListShortcutDir(ExpandEnv(L"%APPDATA%\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar"));
    return pins;
}

struct StartPinsResult { LONG gen; std::vector<Pin> pins; };

static DWORD WINAPI StartPinsThread(LPVOID param) {
    LONG gen = (LONG)(LONG_PTR)param;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    StartPinsResult* res = new StartPinsResult{gen, LoadStartPins()};
    CoUninitialize();
    if (!PostMessageW(g_hwnd, WM_STARTPINS, 0, (LPARAM)res)) delete res;
    return 0;
}

static void RequestStartPins() {
    LONG gen = InterlockedIncrement(&g_startPinsGen);
    if (!g_cfg.startPins) { g_startPins.clear(); return; }
    HANDLE t = CreateThread(nullptr, 0, StartPinsThread, (LPVOID)(LONG_PTR)gen, 0, nullptr);
    if (t) CloseHandle(t);
}

// ---------- config ----------

static const char* kDefaultIni =
"; vbar.ini  - edit, then right-click the clock > Reload\n"
"; All sizes are physical pixels. Comments must be on their own line.\n"
"\n"
"width=120\n"
"; black space between buttons\n"
"spacing=2\n"
"; black gap between the running-windows section, the pinned section and the clock\n"
"gap=6\n"
"; text padding inside a button: pad = left/right, vpad = top/bottom\n"
"pad=4\n"
"vpad=2\n"
"; extra pixels between wrapped lines\n"
"linegap=0\n"
"; wrapped lines per button before cutting off (1 = no wrapping)\n"
"maxlines=3\n"
"; 1 = end a cut-off line with an ellipsis\n"
"ellipsis=1\n"
"; white corner highlight length in px (0 = full top/left edges)\n"
"highlight=4\n"
"\n"
"font=Consolas\n"
"fontsize=12\n"
"; 1 = bold (same as fontweight=700)\n"
"bold=0\n"
"; finer control: 100-900 (400 = normal, 700 = bold, 900 = black)\n"
"fontweight=400\n"
"italic=0\n"
"; optional font file next to vbar.exe, loaded privately (e.g. TerminusTTF-4.49.3.ttf, then font=Terminus (TTF))\n"
"fontfile=\n"
"; 0 = crisp non-antialiased, 1 = ClearType\n"
"aa=0\n"
"clock=1\n"
"; clock text size in px (0 = same as fontsize); it wraps onto up to clocklines lines if it doesn't fit\n"
"clockfontsize=0\n"
"clocklines=2\n"
"; 0 = 12-hour (5:11 PM), 1 = 24-hour (17:11). Also in the right-click menu.\n"
"clock24=0\n"
"; click the clock for a calendar; first day of the week: 0 = Sunday, 1 = Monday\n"
"weekstart=0\n"
"; the calendar stays open while the mouse is over it (or the clock) and closes this many seconds after it leaves\n"
"caltimeout=5\n"
"; clockbold=1   (leave out to follow bold)\n"
"\n"
"; start button at the very bottom: click = Start menu, right-click = Win+X menu\n"
"startbutton=1\n"
"; height in px (0 = same as a one-line button)\n"
"startheight=0\n"
"; colour of the shade blocks (leave out to follow text)\n"
";startcolor=#D8D8D8\n"
"; 1 = hovering a running window's button brings that window to the front\n"
"hoverfocus=1\n"
"; ms to wait before raising (0 = instant)\n"
"hoverdelay=0\n"
"; 1 = hovering also restores minimized windows\n"
"hoverrestore=0\n"
"\n"
"; ---- icons: each app's real icon reduced to 2 bits (transparent + 3 grey shades) ----\n"
"icons=1\n"
"; icon size in px (0 = same as the text line height)\n"
"iconsize=0\n"
"; space between icon and text\n"
"iconpad=3\n"
"; 1 = ordered dither between shades (retro look), 0 = hard steps\n"
"icondither=1\n"
"; the three shades, darkest to lightest\n"
"iconshades=#000000,#8C8C8C,#E8E8E8\n"
"\n"
"; ---- scroll indicator on the inner edge of any section that scrolls ----\n"
"; width in px (0 = off); colour defaults to metercolor\n"
"scrollw=2\n"
";scrollcolor=#2F7DFF\n"
"\n"
"; ---- audio meter: a bar along the bottom of any running window's button that's playing sound ----\n"
"meter=1\n"
"; bar height in px (sits in the bottom padding, so keep vpad >= meterh)\n"
"meterh=2\n"
"metercolor=#2F7DFF\n"
"; dB range shown across the full width (0 = linear amplitude instead)\n"
"meterdb=48\n"
"; how fast the bar falls back, in % of full width per frame\n"
"meterfall=4\n"
"; updates per second while sound is playing\n"
"meterfps=30\n"
"; 1 = hide the Windows taskbar while vbar runs (also turn on taskbar auto-hide so it frees the screen space)\n"
"hidetaskbar=0\n"
"; which screen: primary, or the display number Windows shows in Settings > Display > Identify\n"
"monitor=primary\n"
"; left or right edge of that screen\n"
"side=left\n"
"\n"
"; ---- colours (#RRGGBB) ----\n"
"; bar background (also the space between buttons)\n"
"bg=#000000\n"
"; 1px border around every button\n"
"outline=#000000\n"
"; border colour for running-window buttons picked out by bordermode\n"
"activeborder=#FFFFFF\n"
"; focused = only the window in front | visible = every window not fully covered or minimized | off\n"
"bordermode=focused\n"
"; corner highlight\n"
"hi=#FFFFFF\n"
"; button face; activeface / pinface / clockface override it per kind (leave out to follow face)\n"
"face=#404040\n"
";activeface=#404040\n"
";pinface=#404040\n"
";clockface=#404040\n"
"; text: normal, hovered, focused window, minimized window\n"
"text=#D8D8D8\n"
"hovertext=#FFFFFF\n"
"; how much a hovered button's face brightens (added to each RGB channel; 0 = off)\n"
"hoverbright=28\n"
"activetext=#FFFFFF\n"
"dim=#888888\n"
"; a window asking for attention blinks this colour (leave out to match metercolor)\n"
";flash=#2F7DFF\n"
"; blink speed in ms, and how many blinks before it stays lit until you focus the window (0 = stay lit)\n"
"flashrate=500\n"
"flashcount=8\n"
"\n"
"; suffixes trimmed from window titles\n"
"strip=- Google Chrome\n"
"strip=- Mozilla Firefox\n"
"strip=- Microsoft Edge\n"
"strip=- Brave\n"
"strip=- Visual Studio Code\n"
"strip=- Notepad\n"
"strip=- File Explorer\n"
"\n"
"; ---- pinned section (bottom of the bar) ----\n"
"; 1 = include your Windows Start menu pins (read via Export-StartLayout; falls back to taskbar pins)\n"
"startpins=1\n"
"; most of the bar's height the pinned section may take, in percent (it scrolls past that)\n"
"pinsmax=60\n"
"; your own pins, listed first: pin=Name|path|args|working dir   (env vars like %USERPROFILE% work)\n"
"; pin=Terminal|wt.exe\n"
"; optional folder of shortcuts (.lnk/.url/.exe), listed alphabetically after the pin= lines\n"
"pindir=\n";

static void LoadConfig() {
    g_cfg = Config();
    std::string raw = ReadFileBytes(g_iniPath);
    if (raw.empty()) {
        std::ofstream o(g_iniPath.c_str(), std::ios::binary);
        o << kDefaultIni;
        o.close();
        raw = kDefaultIni;
    }
    std::wstring text = DecodeText(raw);

    std::wstringstream ws(text);
    std::wstring line;
    while (std::getline(ws, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        std::transform(k.begin(), k.end(), k.begin(), ::towlower);
        int n = _wtoi(v.c_str());
        if      (k == L"width")       g_cfg.width = std::max(20, n);
        else if (k == L"gap")         g_cfg.gap = std::max(0, n);
        else if (k == L"spacing")     g_cfg.spacing = std::max(0, n);
        else if (k == L"pad")         g_cfg.pad = std::max(0, n);
        else if (k == L"vpad")        g_cfg.vpad = std::max(0, n);
        else if (k == L"linegap")     g_cfg.lineGap = std::max(0, n);
        else if (k == L"maxlines")    g_cfg.maxLines = std::max(1, n);
        else if (k == L"ellipsis")    g_cfg.ellipsis = n != 0;
        else if (k == L"highlight")   g_cfg.highlight = std::max(0, n);
        else if (k == L"font")        { if (!v.empty()) g_cfg.font = v; }
        else if (k == L"fontsize")    g_cfg.fontSize = std::max(5, n);
        else if (k == L"fontweight")  g_cfg.fontWeight = std::max(100, std::min(900, n));
        else if (k == L"italic")      g_cfg.italic = n != 0;
        else if (k == L"bold")        g_cfg.bold = n != 0;
        else if (k == L"hoverfocus")  g_cfg.hoverFocus = n != 0;
        else if (k == L"hoverdelay")  g_cfg.hoverDelay = std::max(0, n);
        else if (k == L"activeborder") g_cfg.activeBorder = ParseColor(v, g_cfg.activeBorder);
        else if (k == L"bordermode") {
            std::wstring m = v;
            std::transform(m.begin(), m.end(), m.begin(), ::towlower);
            g_cfg.borderMode = m == L"off" ? 0 : m == L"visible" ? 2 : 1;
        }
        else if (k == L"hoverrestore") g_cfg.hoverRestore = n != 0;
        else if (k == L"meter")       g_cfg.meter = n != 0;
        else if (k == L"meterh")      g_cfg.meterH = std::max(1, n);
        else if (k == L"meterfall")   g_cfg.meterFall = std::max(1, std::min(100, n));
        else if (k == L"meterdb")     g_cfg.meterDb = std::max(0, n);
        else if (k == L"meterfps")    g_cfg.meterFps = std::max(5, std::min(120, n));
        else if (k == L"metercolor")  g_cfg.meterColor = ParseColor(v, g_cfg.meterColor);
        else if (k == L"outline")     g_cfg.outline = ParseColor(v, g_cfg.outline);
        else if (k == L"hovertext")   g_cfg.hoverText = ParseColor(v, g_cfg.hoverText);
        else if (k == L"activetext")  g_cfg.activeText = ParseColor(v, g_cfg.activeText);
        else if (k == L"activeface")  { g_cfg.activeFace = ParseColor(v, g_cfg.activeFace); g_cfg.activeFaceSet = true; }
        else if (k == L"pinface")     { g_cfg.pinFace = ParseColor(v, g_cfg.pinFace); g_cfg.pinFaceSet = true; }
        else if (k == L"clockface")   { g_cfg.clockFace = ParseColor(v, g_cfg.clockFace); g_cfg.clockFaceSet = true; }
        else if (k == L"fontfile")    g_cfg.fontFile = v;
        else if (k == L"aa")          g_cfg.aa = n != 0;
        else if (k == L"clock")       g_cfg.clock = n != 0;
        else if (k == L"clockfontsize") g_cfg.clockFontSize = std::max(0, n);
        else if (k == L"clocklines")  g_cfg.clockLines = std::max(1, n);
        else if (k == L"clockbold")   g_cfg.clockBold = n != 0;
        else if (k == L"clock24")     g_cfg.clock24 = n != 0;
        else if (k == L"caltimeout")  g_cfg.calTimeout = std::max(1, n);
        else if (k == L"weekstart")   g_cfg.weekStart = (n == 1) ? 1 : 0;
        else if (k == L"startbutton") g_cfg.startButton = n != 0;
        else if (k == L"startheight") g_cfg.startHeight = std::max(0, n);
        else if (k == L"startcolor")  { g_cfg.startColor = ParseColor(v, g_cfg.startColor); g_cfg.startColorSet = true; }
        else if (k == L"hidetaskbar") g_cfg.hideTaskbar = n != 0;
        else if (k == L"side") {
            std::wstring m = v;
            std::transform(m.begin(), m.end(), m.begin(), ::towlower);
            g_cfg.rightSide = m == L"right";
        }
        else if (k == L"monitor")     g_cfg.monitor = std::max(0, n);   // "primary" parses as 0
        else if (k == L"startpins")   g_cfg.startPins = n != 0;
        else if (k == L"pinsmax")     g_cfg.pinsMax = std::max(10, std::min(100, n));
        else if (k == L"bg")          g_cfg.bg = ParseColor(v, g_cfg.bg);
        else if (k == L"face")        g_cfg.face = ParseColor(v, g_cfg.face);
        else if (k == L"text")        g_cfg.text = ParseColor(v, g_cfg.text);
        else if (k == L"hi")          g_cfg.hi = ParseColor(v, g_cfg.hi);
        else if (k == L"dim")         g_cfg.dim = ParseColor(v, g_cfg.dim);
        else if (k == L"flash")       { g_cfg.flashColor = ParseColor(v, g_cfg.flashColor); g_cfg.flashColorSet = true; }
        else if (k == L"flashrate")   g_cfg.flashRate = std::max(100, n);
        else if (k == L"flashcount")  g_cfg.flashCount = std::max(0, n);
        else if (k == L"icons")       g_cfg.icons = n != 0;
        else if (k == L"iconsize")    g_cfg.iconSize = std::max(0, n);
        else if (k == L"iconpad")     g_cfg.iconPad = std::max(0, n);
        else if (k == L"icondither")  g_cfg.iconDither = n != 0;
        else if (k == L"iconshades") {
            auto parts = Split(v, L',');
            for (size_t j = 0; j < parts.size() && j < 3; ++j) g_cfg.iconShade[j] = ParseColor(parts[j], g_cfg.iconShade[j]);
        }
        else if (k == L"scrollw")     g_cfg.scrollW = std::max(0, n);
        else if (k == L"scrollcolor") { g_cfg.scrollColor = ParseColor(v, g_cfg.scrollColor); g_cfg.scrollColorSet = true; }
        else if (k == L"hoverbright") g_cfg.hoverBright = std::max(-255, std::min(255, n));
        else if (k == L"strip")       { if (!v.empty()) g_cfg.strip.push_back(v); }
        else if (k == L"pindir")      g_cfg.pinDir = v;
        else if (k == L"pin") {
            auto parts = Split(v, L'|');
            Pin p;
            p.name = parts.size() > 0 ? parts[0] : L"";
            p.path = parts.size() > 1 ? parts[1] : L"";
            p.args = parts.size() > 2 ? parts[2] : L"";
            p.dir  = parts.size() > 3 ? parts[3] : L"";
            if (!p.name.empty() && !p.path.empty()) g_cfg.pins.push_back(p);
        }
    }

    if (!g_cfg.startColorSet) g_cfg.startColor = g_cfg.text;
    if (!g_cfg.flashColorSet) g_cfg.flashColor = g_cfg.meterColor;
    if (!g_cfg.scrollColorSet) g_cfg.scrollColor = g_cfg.meterColor;
    if (!g_cfg.activeFaceSet) g_cfg.activeFace = g_cfg.face;
    if (!g_cfg.pinFaceSet)    g_cfg.pinFace = g_cfg.face;
    if (!g_cfg.clockFaceSet)  g_cfg.clockFace = g_cfg.face;

    if (!g_cfg.pinDir.empty()) {
        auto found = ListShortcutDir(Resolve(g_cfg.pinDir));
        g_cfg.pins.insert(g_cfg.pins.end(), found.begin(), found.end());
    }
}

static void CreateBarFont() {
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    if (g_clockFont) { DeleteObject(g_clockFont); g_clockFont = nullptr; }
    if (g_calBigFont) { DeleteObject(g_calBigFont); g_calBigFont = nullptr; }
    if (!g_loadedFontFile.empty()) {
        RemoveFontResourceExW(g_loadedFontFile.c_str(), FR_PRIVATE, nullptr);
        g_loadedFontFile.clear();
    }
    if (!g_cfg.fontFile.empty()) {
        std::wstring p = Resolve(g_cfg.fontFile);
        if (AddFontResourceExW(p.c_str(), FR_PRIVATE, nullptr) > 0) g_loadedFontFile = p;
    }
    int weight = g_cfg.bold ? std::max(g_cfg.fontWeight, (int)FW_BOLD) : g_cfg.fontWeight;
    g_font = CreateFontW(-g_cfg.fontSize, 0, 0, 0, weight, g_cfg.italic, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         g_cfg.aa ? CLEARTYPE_QUALITY : NONANTIALIASED_QUALITY,
                         FIXED_PITCH | FF_MODERN, g_cfg.font.c_str());
    bool cb = g_cfg.clockBold < 0 ? g_cfg.bold : g_cfg.clockBold != 0;
    int cweight = cb ? std::max(g_cfg.fontWeight, (int)FW_BOLD) : g_cfg.fontWeight;
    g_clockFont = CreateFontW(-(g_cfg.clockFontSize ? g_cfg.clockFontSize : g_cfg.fontSize), 0, 0, 0, cweight,
                              g_cfg.italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              g_cfg.aa ? CLEARTYPE_QUALITY : NONANTIALIASED_QUALITY,
                              FIXED_PITCH | FF_MODERN, g_cfg.font.c_str());
    g_calBigFont = CreateFontW(-g_cfg.fontSize * 2, 0, 0, 0, cweight, g_cfg.italic, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               g_cfg.aa ? CLEARTYPE_QUALITY : NONANTIALIASED_QUALITY,
                               FIXED_PITCH | FF_MODERN, g_cfg.font.c_str());
}

// ---------- taskbar / appbar ----------

static void ShowSystemTaskbars(bool show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    if (HWND t = FindWindowW(L"Shell_TrayWnd", nullptr)) ShowWindow(t, cmd);
    HWND s = nullptr;
    while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) ShowWindow(s, cmd);
}

struct Mon { HMONITOR h; RECT rc; int num; bool primary; };

static BOOL CALLBACK MonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    std::wstring dev = mi.szDevice;                       // e.g. \\.\DISPLAY2
    size_t d = dev.find_last_not_of(L"0123456789");
    int num = (d != std::wstring::npos && d + 1 < dev.size()) ? _wtoi(dev.c_str() + d + 1) : 0;
    ((std::vector<Mon>*)lp)->push_back(Mon{hm, mi.rcMonitor, num, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

static std::vector<Mon> Monitors() {
    std::vector<Mon> v;
    EnumDisplayMonitors(nullptr, nullptr, MonProc, (LPARAM)&v);
    std::sort(v.begin(), v.end(), [](const Mon& a, const Mon& b) { return a.num < b.num; });
    return v;
}

// The configured screen, or the primary one if it isn't connected.
static HMONITOR PickMonitor() {
    if (g_cfg.monitor > 0)
        for (auto& m : Monitors()) if (m.num == g_cfg.monitor) return m.h;
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

static void AppBarSetPos() {
    HMONITOR mon = PickMonitor();
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = g_hwnd;
    abd.uEdge = g_cfg.rightSide ? ABE_RIGHT : ABE_LEFT;
    abd.rc = mi.rcMonitor;
    if (g_cfg.rightSide) abd.rc.left = abd.rc.right - g_cfg.width;
    else abd.rc.right = abd.rc.left + g_cfg.width;
    SHAppBarMessage(ABM_QUERYPOS, &abd);                 // the shell may move our edge in (other appbars)
    if (g_cfg.rightSide) abd.rc.left = abd.rc.right - g_cfg.width;
    else abd.rc.right = abd.rc.left + g_cfg.width;
    SHAppBarMessage(ABM_SETPOS, &abd);

    SetWindowPos(g_hwnd, HWND_TOPMOST, abd.rc.left, abd.rc.top,
                 abd.rc.right - abd.rc.left, abd.rc.bottom - abd.rc.top, SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void AppBarRegister() {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = g_hwnd;
    abd.uCallbackMessage = WM_APPBAR;
    SHAppBarMessage(ABM_NEW, &abd);
    AppBarSetPos();
}

static void AppBarRemove() {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = g_hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd);
}

// ---------- window list ----------

static bool IsTaskWindow(HWND h) {
    if (h == g_hwnd || !IsWindowVisible(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    bool appWin = (ex & WS_EX_APPWINDOW) != 0;
    if (!appWin) {
        if (GetWindow(h, GW_OWNER)) return false;
        if (ex & WS_EX_TOOLWINDOW) return false;
        if (ex & WS_EX_NOACTIVATE) return false;
    }
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return false;
    return true;
}

static std::wstring CleanTitle(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring t(len + 1, 0);
    GetWindowTextW(h, &t[0], len + 1);
    t.resize(wcslen(t.c_str()));
    t.erase(std::remove(t.begin(), t.end(), L'​'), t.end());   // Edge pads its name with zero-width spaces
    for (auto& s : g_cfg.strip) {
        if (t.size() > s.size() && t.compare(t.size() - s.size(), s.size(), s) == 0) {
            t.resize(t.size() - s.size());
            t = Trim(t);
            break;
        }
    }
    return t;
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    if (IsTaskWindow(h)) ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}

// Rebuilds the task list keeping existing order; returns true if anything visible changed.
static bool RefreshTasks() {
    std::vector<HWND> found;
    EnumWindows(EnumProc, (LPARAM)&found);
    HWND fg = GetForegroundWindow();

    std::vector<Task> next;
    for (auto& t : g_tasks)
        if (std::find(found.begin(), found.end(), t.hwnd) != found.end()) next.push_back(t);
    for (HWND h : found) {
        bool have = std::any_of(next.begin(), next.end(), [h](const Task& t) { return t.hwnd == h; });
        if (!have) {
            Task t{h, L"", false, false, false};
            GetWindowThreadProcessId(h, &t.pid);
            t.exe = ProcessPath(h);
            next.push_back(t);
        }
    }

    // Which windows show any pixels: walk top-to-bottom z-order, subtracting the windows above.
    std::vector<HWND> shown;
    if (g_cfg.borderMode == 2) {
        RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        std::vector<RECT> above;
        for (HWND h : found) {                       // EnumWindows returns top-most first
            if (IsIconic(h)) continue;
            RECT wr;
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr)))) GetWindowRect(h, &wr);
            RECT vis;
            if (!IntersectRect(&vis, &wr, &screen)) continue;
            HRGN rg = CreateRectRgnIndirect(&vis);
            int res = SIMPLEREGION;
            for (auto& a : above) {
                HRGN ar = CreateRectRgnIndirect(&a);
                res = CombineRgn(rg, rg, ar, RGN_DIFF);
                DeleteObject(ar);
                if (res == NULLREGION) break;
            }
            DeleteObject(rg);
            if (res != NULLREGION) shown.push_back(h);
            above.push_back(wr);
        }
    }

    for (auto& t : next) {
        t.title = CleanTitle(t.hwnd);
        t.iconic = IsIconic(t.hwnd) != 0;
        t.visible = std::find(shown.begin(), shown.end(), t.hwnd) != shown.end();
        if (t.hwnd == fg) t.flash = false;
    }

    bool changed = next.size() != g_tasks.size() || fg != g_lastFg;
    for (size_t i = 0; !changed && i < next.size(); ++i)
        changed = next[i].hwnd != g_tasks[i].hwnd || next[i].title != g_tasks[i].title ||
                  next[i].iconic != g_tasks[i].iconic || next[i].flash != g_tasks[i].flash ||
                  next[i].visible != g_tasks[i].visible;
    g_tasks.swap(next);
    g_lastFg = fg;
    return changed;
}

// ---------- text wrapping ----------

static bool IsBreakAfter(wchar_t c) { return c == L'-' || c == L'/' || c == L'\\' || c == L'_' || c == L'|'; }

// Wraps at spaces (or after - / \ _ |), hard-breaks words longer than a line, caps at maxLines.
// The first narrowLines lines get narrowW (they sit beside the icon); the rest get the full maxW.
static std::vector<std::wstring> Wrap(HDC dc, const std::wstring& s, int maxW, int maxLines,
                                      int narrowW = 0, int narrowLines = 0) {
    std::vector<std::wstring> out;
    size_t pos = 0, n = s.size();
    while (pos < n && (int)out.size() < maxLines) {
        while (pos < n && s[pos] == L' ') ++pos;
        if (pos >= n) break;
        int fit = 0;
        SIZE sz;
        int lineW = (int)out.size() < narrowLines ? narrowW : maxW;
        GetTextExtentExPointW(dc, s.c_str() + pos, (int)(n - pos), lineW, &fit, nullptr, &sz);
        if (fit < 1) fit = 1;
        if ((size_t)fit >= n - pos) { out.push_back(Trim(s.substr(pos))); break; }

        if ((int)out.size() == maxLines - 1) {          // last allowed line: cut it
            std::wstring l = s.substr(pos, fit);
            if (g_cfg.ellipsis && l.size() > 1) { l.pop_back(); l += L'…'; }
            out.push_back(l);
            break;
        }

        size_t end = pos + fit, next = end;
        if (s[end] == L' ') {
            next = end + 1;
        } else {
            for (size_t k = end; k > pos + 1; --k) {
                if (s[k - 1] == L' ')          { end = k - 1; next = k; break; }
                if (IsBreakAfter(s[k - 1]))    { end = k;     next = k; break; }
            }
        }
        std::wstring l = s.substr(pos, end - pos);
        while (!l.empty() && l.back() == L' ') l.pop_back();
        out.push_back(l);
        pos = next;
    }
    if (out.empty()) out.push_back(L"");
    return out;
}

// ---------- icons ----------

static BITMAPINFO Dib32(int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return bi;
}

// px: s*s straight-alpha BGRA. Contrast-stretches the icon's own brightness range, then maps it
// to 3 shades (optionally Bayer-dithered); pixels under 50% alpha become transparent.
static IconPtr MakeIcon2(const std::vector<uint32_t>& px, int s) {
    if (s <= 0 || (int)px.size() != s * s) return nullptr;
    std::vector<int> lum(s * s, -1);
    int lo = 255, hi = 0;
    for (int i = 0; i < s * s; ++i) {
        uint32_t p = px[i];
        if ((int)(p >> 24) < 128) continue;
        int b = p & 255, g = (p >> 8) & 255, r = (p >> 16) & 255;
        int l = (r * 299 + g * 587 + b * 114) / 1000;
        lum[i] = l;
        lo = std::min(lo, l);
        hi = std::max(hi, l);
    }
    if (hi < lo) return nullptr;                         // nothing opaque
    uint32_t shade[3];
    for (int k = 0; k < 3; ++k) {
        COLORREF c = g_cfg.iconShade[k];
        shade[k] = 0xFF000000u | ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c);
    }
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    BITMAPINFO bi = Dib32(s, s);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return nullptr;
    uint32_t* out = (uint32_t*)bits;
    bool flat = hi - lo < 48;                            // one-colour glyph: show it in the light shade
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            int i = y * s + x;
            if (lum[i] < 0) { out[i] = 0; continue; }
            float t = flat ? 1.0f : (lum[i] - lo) / (float)(hi - lo);
            float d = g_cfg.iconDither ? (bayer[y & 3][x & 3] + 0.5f) / 16.0f : 0.5f;
            int q = std::max(0, std::min(2, (int)(t * 2.0f + d)));
            out[i] = shade[q];                           // opaque, so premultiplied == straight
        }
    GdiFlush();
    IconPtr ip = std::make_shared<Icon2>();
    ip->bmp = bmp;
    ip->size = s;
    return ip;
}

// Renders an HICON onto black and onto white; the difference recovers alpha for any icon format.
static std::vector<uint32_t> RasterHIcon(HICON hi, int s) {
    std::vector<uint32_t> out;
    if (!hi || s <= 0) return out;
    BITMAPINFO bi = Dib32(s, s);
    void *pb = nullptr, *pw = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pb, nullptr, 0);
    HBITMAP bw = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pw, nullptr, 0);
    if (bb && bw && pb && pw) {
        memset(pb, 0x00, (size_t)s * s * 4);
        memset(pw, 0xFF, (size_t)s * s * 4);
        HGDIOBJ old = SelectObject(dc, bb);
        DrawIconEx(dc, 0, 0, hi, s, s, 0, nullptr, DI_NORMAL);
        SelectObject(dc, bw);
        DrawIconEx(dc, 0, 0, hi, s, s, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        GdiFlush();
        const uint32_t* B = (const uint32_t*)pb;
        const uint32_t* Wt = (const uint32_t*)pw;
        out.resize((size_t)s * s);
        for (int i = 0; i < s * s; ++i) {
            int a = 0;
            for (int ch = 0; ch < 3; ++ch)
                a += 255 - ((int)((Wt[i] >> (8 * ch)) & 255) - (int)((B[i] >> (8 * ch)) & 255));
            a = std::max(0, std::min(255, a / 3));
            uint32_t px = (uint32_t)a << 24;
            for (int ch = 0; ch < 3; ++ch) {
                int vb = (B[i] >> (8 * ch)) & 255;
                int c = a ? std::min(255, vb * 255 / a) : 0;
                px |= (uint32_t)c << (8 * ch);
            }
            out[i] = px;
        }
    }
    if (bb) DeleteObject(bb);
    if (bw) DeleteObject(bw);
    DeleteDC(dc);
    return out;
}

// Store apps: ask the shell for the app's icon image.
static std::vector<uint32_t> RasterShellItem(const std::wstring& parse, int s) {
    std::vector<uint32_t> out;
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(parse.c_str(), nullptr, IID_IShellItemImageFactory, (void**)&f)) || !f)
        return out;
    HBITMAP hb = nullptr;
    SIZE want{s, s};
    if (SUCCEEDED(f->GetImage(want, SIIGBF_ICONONLY, &hb)) && hb) {
        BITMAP bm{};
        GetObjectW(hb, sizeof(bm), &bm);
        int w = bm.bmWidth, h = bm.bmHeight;
        if (w > 0 && h > 0) {
            std::vector<uint32_t> src((size_t)w * h);
            BITMAPINFO bi = Dib32(w, h);
            HDC sdc = GetDC(nullptr);
            int got = GetDIBits(sdc, hb, 0, h, src.data(), &bi, DIB_RGB_COLORS);
            ReleaseDC(nullptr, sdc);
            if (got > 0) {
                bool anyAlpha = false, straight = false;
                for (uint32_t p : src) {
                    int a = p >> 24;
                    if (a) anyAlpha = true;
                    if ((int)(p & 255) > a || (int)((p >> 8) & 255) > a || (int)((p >> 16) & 255) > a) straight = true;
                }
                out.resize((size_t)s * s);
                for (int y = 0; y < s; ++y)
                    for (int x = 0; x < s; ++x) {
                        uint32_t p = src[(size_t)(y * h / s) * w + (x * w / s)];
                        int a = anyAlpha ? (int)(p >> 24) : 255;
                        uint32_t px = (uint32_t)a << 24;
                        for (int ch = 0; ch < 3; ++ch) {
                            int c = (p >> (8 * ch)) & 255;
                            if (anyAlpha && !straight && a > 0) c = std::min(255, c * 255 / a);
                            px |= (uint32_t)c << (8 * ch);
                        }
                        out[(size_t)y * s + x] = px;
                    }
            }
        }
        DeleteObject(hb);
    }
    f->Release();
    return out;
}

static IconPtr IconFromFile(const std::wstring& path, int s) {
    if (path.empty()) return nullptr;
    SHFILEINFOW sfi{};
    UINT flags = SHGFI_ICON | (s > 16 ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags) || !sfi.hIcon) return nullptr;
    IconPtr ip = MakeIcon2(RasterHIcon(sfi.hIcon, s), s);
    DestroyIcon(sfi.hIcon);
    return ip;
}

// Bare names like "explorer.exe" are looked up on PATH.
static std::wstring PinFilePath(const Pin& p) {
    std::wstring e = ExpandEnv(p.path);
    if (e.find(L'\\') != std::wstring::npos || e.find(L'/') != std::wstring::npos) return e;
    if (e.find(L':') != std::wstring::npos) return L"";           // a URI like ms-settings:
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, e.c_str(), nullptr, MAX_PATH, buf, nullptr)) return buf;
    return Resolve(e);
}

static IconPtr PinIcon(const Pin& p, int s) {
    std::wstring key = p.aumid.empty() ? L"path:" + p.path : L"app:" + p.aumid;
    auto it = g_pinIcons.find(key);
    if (it != g_pinIcons.end()) return it->second;       // also caches "no icon"
    IconPtr ip = p.aumid.empty() ? IconFromFile(PinFilePath(p), s)
                                 : MakeIcon2(RasterShellItem(L"shell:AppsFolder\\" + p.aumid, s), s);
    g_pinIcons[key] = ip;
    return ip;
}

static HICON WindowIconHandle(HWND h, bool big) {
    const WPARAM order[3] = {big ? (WPARAM)ICON_BIG : (WPARAM)ICON_SMALL2, big ? (WPARAM)ICON_SMALL2 : (WPARAM)ICON_BIG,
                             (WPARAM)ICON_SMALL};
    for (WPARAM w : order) {
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(h, WM_GETICON, w, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &res) && res) return (HICON)res;
    }
    ULONG_PTR c = GetClassLongPtrW(h, big ? GCLP_HICON : GCLP_HICONSM);
    if (!c) c = GetClassLongPtrW(h, big ? GCLP_HICONSM : GCLP_HICON);
    return (HICON)c;
}

// The window's own icon; falls back to its exe's icon and retries for a few seconds,
// since many apps set their window icon only after the window appears.
static void EnsureTaskIcon(Task& t, int s) {
    if (t.iconFinal) return;
    DWORD now = GetTickCount();
    if (t.iconTries > 0 && (LONG)(now - t.iconNext) < 0) return;
    HICON hi = WindowIconHandle(t.hwnd, s > 16);
    IconPtr ip = hi ? MakeIcon2(RasterHIcon(hi, s), s) : nullptr;    // window icons are not ours to destroy
    if (ip) {
        t.icon = ip;
        t.iconFinal = true;
        return;
    }
    if (!t.icon) t.icon = IconFromFile(t.exe, s);
    if (++t.iconTries >= 6) t.iconFinal = true;
    t.iconNext = now + 1000;
}

static void ResetIcons() {
    g_pinIcons.clear();
    for (auto& t : g_tasks) { t.icon.reset(); t.iconTries = 0; t.iconFinal = false; t.iconNext = 0; }
}

// ---------- drawing ----------

// Text lines that overlap the icon vertically are indented past it; lines below wrap under it.
static int IndentLines() {
    if (!g_cfg.icons || g_iconS <= 0) return 0;
    int iconTop = g_iconS <= g_lineH ? (g_lineH - g_iconS) / 2 : 0;
    int iconBottom = iconTop + g_iconS + g_cfg.lineGap;      // keep at least linegap below the icon
    int k = 0;
    while (k * (g_lineH + g_cfg.lineGap) < iconBottom) ++k;
    return k;
}

static int ButtonHeight(int lines) {
    return 2 + 2 * g_cfg.vpad + lines * g_lineH + (lines - 1) * g_cfg.lineGap;
}

static void DrawButton(HDC dc, RECT r, bool pressed, COLORREF border, COLORREF face, COLORREF txt,
                       const std::vector<std::wstring>& lines, const Icon2* icon = nullptr,
                       int indent = 0, BYTE iconAlpha = 255) {
    Fill(dc, r, border);                                // 1px outline
    RECT in{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    if (in.right <= in.left || in.bottom <= in.top) return;
    Fill(dc, in, face);

    int w = in.right - in.left, h = in.bottom - in.top;
    int lx = g_cfg.highlight ? std::min(g_cfg.highlight, w) : w;
    int ly = g_cfg.highlight ? std::min(g_cfg.highlight, h) : h;
    if (!pressed) {                                     // raised: light from top-left
        Fill(dc, RECT{in.left, in.top, in.left + lx, in.top + 1}, g_cfg.hi);
        Fill(dc, RECT{in.left, in.top, in.left + 1, in.top + ly}, g_cfg.hi);
    } else {                                            // sunken: light lands bottom-right
        Fill(dc, RECT{in.right - lx, in.bottom - 1, in.right, in.bottom}, g_cfg.hi);
        Fill(dc, RECT{in.right - 1, in.bottom - ly, in.right, in.bottom}, g_cfg.hi);
    }

    int shift = pressed ? 1 : 0;
    RECT clip{in.left + 1, in.top + 1, in.right - 1, in.bottom - 1};
    SetTextColor(dc, txt);
    int x = in.left + g_cfg.pad + shift;
    int y = in.top + g_cfg.vpad + shift;
    if (icon && icon->bmp) {
        if (!g_iconDC) g_iconDC = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(g_iconDC, icon->bmp);
        int is = icon->size;
        int iy = is <= g_lineH ? y + (g_lineH - is) / 2 : y;     // centred on the first text line
        BLENDFUNCTION bf{AC_SRC_OVER, 0, iconAlpha, AC_SRC_ALPHA};
        AlphaBlend(dc, x, iy, is, is, g_iconDC, 0, 0, is, is, bf);
        SelectObject(g_iconDC, old);
    }
    int beside = indent ? IndentLines() : 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& l = lines[i];
        int lx = x + ((int)i < beside ? indent : 0);
        ExtTextOutW(dc, lx, y, ETO_CLIPPED, &clip, l.c_str(), (UINT)l.size(), nullptr);
        y += g_lineH + g_cfg.lineGap;
    }
}

static std::wstring ClockText() {
    static const wchar_t* D[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    static const wchar_t* M[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                 L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    if (g_cfg.clock24)
        wsprintfW(buf, L"%02d:%02d %s %d %s", st.wHour, st.wMinute, D[st.wDayOfWeek], st.wDay, M[st.wMonth - 1]);
    else
        wsprintfW(buf, L"%d:%02d %s %s %d %s", (st.wHour + 11) % 12 + 1, st.wMinute, st.wHour < 12 ? L"AM" : L"PM",
                  D[st.wDayOfWeek], st.wDay, M[st.wMonth - 1]);
    return buf;
}

static int ContentHeight(const std::vector<Item>& v) {
    if (v.empty()) return 0;
    int h = 0;
    for (auto& it : v) h += it.h;
    return h + g_cfg.spacing * ((int)v.size() - 1);
}

static COLORREF Brighten(COLORREF c, int d) {
    auto ch = [d](int v) { return std::max(0, std::min(255, v + d)); };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

static COLORREF HoverFace(COLORREF face, bool hov) { return hov ? Brighten(face, g_cfg.hoverBright) : face; }

// Buttons span x0..x1; sbX >= 0 draws a scroll thumb at that x when content overflows.
static void DrawRegion(HDC dc, int W, int x0, int x1, const std::vector<Item>& items, int top, int bottom,
                       int scroll, HWND fg, int content, int sbX) {
    if (bottom <= top) return;
    HRGN clip = CreateRectRgn(0, top, W, bottom);
    SelectClipRgn(dc, clip);
    int y = top - scroll;
    for (auto& it : items) {
        RECT r{x0, y, x1, y + it.h};
        if (r.bottom > top && r.top < bottom) {
            bool hov = g_hoverKind == it.kind && g_hoverIdx == it.idx;
            if (it.kind == K_TASK) {
                const Task& t = g_tasks[it.idx];
                bool active = t.hwnd == fg && !t.iconic;
                bool blink = t.flash && g_blinkOn;
                COLORREF face = HoverFace(blink ? g_cfg.flashColor : (active ? g_cfg.activeFace : g_cfg.face), hov);
                COLORREF txt = (active || blink) ? g_cfg.activeText : hov ? g_cfg.hoverText : (t.iconic ? g_cfg.dim : g_cfg.text);
                bool lit = (g_cfg.borderMode == 1 && active) || (g_cfg.borderMode == 2 && (active || t.visible));
                DrawButton(dc, r, active, lit ? g_cfg.activeBorder : g_cfg.outline, face, txt, it.lines,
                           g_cfg.icons ? t.icon.get() : nullptr, g_indent, t.iconic ? 120 : 255);
                if (g_cfg.meter && t.level > 0.001f) {
                    int x0 = r.left + 1, x1 = r.right - 1;
                    int w = (int)((x1 - x0) * std::min(1.0f, t.level) + 0.5f);
                    if (w > 0) Fill(dc, RECT{x0, r.bottom - 1 - g_cfg.meterH, x0 + w, r.bottom - 1}, g_cfg.meterColor);
                }
            } else {
                IconPtr ic = g_cfg.icons && it.idx < PinCount() ? PinIcon(PinAt(it.idx), g_iconS) : nullptr;
                DrawButton(dc, r, false, g_cfg.outline, HoverFace(g_cfg.pinFace, hov), hov ? g_cfg.hoverText : g_cfg.text,
                           it.lines, ic.get(), g_indent);
            }
        }
        g_rows.push_back(Row{r, it.kind, it.idx, top, bottom});
        y += it.h + g_cfg.spacing;
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    int area = bottom - top;
    if (sbX >= 0 && g_cfg.scrollW > 0 && content > area) {
        int th = std::max(std::min(area, 8), (int)((long long)area * area / content));
        int ty = top + (int)((long long)(area - th) * scroll / std::max(1, content - area));
        Fill(dc, RECT{sbX, ty, sbX + g_cfg.scrollW, ty + th}, g_cfg.scrollColor);
    }
}

// ░▒▓█ drawn as real pixel dither patterns, stretched across the button, so it's crisp in any font.
static void DrawStartButton(HDC dc, RECT r, COLORREF border, COLORREF face, COLORREF ink) {
    DrawButton(dc, r, false, border, face, ink, {});
    RECT a{r.left + 1 + g_cfg.pad, r.top + 1 + g_cfg.vpad, r.right - 1 - g_cfg.pad, r.bottom - 1 - g_cfg.vpad};
    int w = a.right - a.left;
    if (w < 4 || a.bottom <= a.top) return;
    for (int c = 0; c < 4; ++c) {
        int x0 = a.left + w * c / 4, x1 = a.left + w * (c + 1) / 4;
        if (c == 3) { Fill(dc, RECT{x0, a.top, x1, a.bottom}, ink); continue; }
        for (int y = a.top; y < a.bottom; ++y)
            for (int x = x0; x < x1; ++x) {
                bool on = c == 0 ? (x % 2 == 0 && y % 2 == 0)       // ░ 25%
                        : c == 1 ? ((x + y) % 2 == 0)                // ▒ 50%
                        :          !(x % 2 == 1 && y % 2 == 1);      // ▓ 75%
                if (on) SetPixelV(dc, x, y, ink);
            }
    }
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    int W = cr.right, H = cr.bottom;

    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mdc, bmp);
    HGDIOBJ oldFont = SelectObject(mdc, g_font);
    SetBkMode(mdc, TRANSPARENT);
    Fill(mdc, cr, g_cfg.bg);

    TEXTMETRICW tm;
    GetTextMetricsW(mdc, &tm);
    g_lineH = tm.tmHeight;
    int textW = std::max(1, W - 2 - 2 * g_cfg.pad - 1);   // -1 leaves room for the pressed shift

    // Icons: size follows the text line unless set; text is indented past the icon on every line.
    int iconS = g_cfg.iconSize ? g_cfg.iconSize : g_lineH;
    if (g_cfg.icons && iconS != g_iconS) ResetIcons();
    g_iconS = iconS;
    g_indent = g_cfg.icons ? iconS + g_cfg.iconPad : 0;
    int minH = g_cfg.icons ? 2 + 2 * g_cfg.vpad + iconS : 0;
    if (g_cfg.icons) for (auto& t : g_tasks) EnsureTaskIcon(t, iconS);

    auto build = [&](int kind, int bw) {
        std::vector<Item> v;
        int fullW = std::max(1, bw - 2 - 2 * g_cfg.pad - 1);
        int tw = std::max(1, fullW - g_indent);
        int beside = g_indent ? IndentLines() : 0;
        int n = kind == K_TASK ? (int)g_tasks.size() : PinCount();
        for (int i = 0; i < n; ++i) {
            auto lines = Wrap(mdc, kind == K_TASK ? g_tasks[i].title : PinAt(i).name, fullW, g_cfg.maxLines,
                              tw, beside);
            v.push_back(Item{kind, i, lines, std::max(ButtonHeight((int)lines.size()), minH)});
        }
        return v;
    };
    int sbW = g_cfg.scrollW, sbGap = sbW ? 1 : 0;
    int narrowW = std::max(4, W - sbW - sbGap);

    // Clock: own font size, wraps if it doesn't fit the bar width.
    std::vector<std::wstring> clockLines;
    int clockLineH = g_lineH;
    if (g_cfg.clock) {
        SelectObject(mdc, g_clockFont);
        TEXTMETRICW ctm;
        GetTextMetricsW(mdc, &ctm);
        clockLineH = ctm.tmHeight;
        clockLines = Wrap(mdc, ClockText(), textW, g_cfg.clockLines);
        SelectObject(mdc, g_font);
    }

    // Layout, top to bottom:
    // [running windows ... scrolls] gap [pinned ... anchored low] gap [clock] spacing [start]
    int startH = g_cfg.startButton ? (g_cfg.startHeight ? g_cfg.startHeight : ButtonHeight(1)) : 0;
    int startTop = H - startH;
    int clockH = g_cfg.clock ? 2 + 2 * g_cfg.vpad + (int)clockLines.size() * clockLineH +
                               ((int)clockLines.size() - 1) * g_cfg.lineGap : 0;
    int clockTop = (g_cfg.startButton ? startTop - g_cfg.spacing : H) - clockH;
    int bottomLimit = g_cfg.clock ? clockTop - g_cfg.gap
                    : g_cfg.startButton ? startTop - g_cfg.gap : H;

    // A section that overflows is rewrapped a little narrower to make room for its scroll indicator.
    int pinsMaxArea = std::max(0, bottomLimit) * g_cfg.pinsMax / 100;
    std::vector<Item> pins = build(K_PIN, W);
    bool pinsScroll = sbW > 0 && ContentHeight(pins) > pinsMaxArea;
    if (pinsScroll) pins = build(K_PIN, narrowW);
    int pinsContent = ContentHeight(pins);
    int pinsArea = pins.empty() ? 0 : std::min(pinsContent, pinsMaxArea);
    g_pinsBottom = bottomLimit;
    g_pinsTop = bottomLimit - pinsArea;
    g_tasksTop = 0;
    g_tasksBottom = pins.empty() ? bottomLimit : g_pinsTop - g_cfg.gap;

    std::vector<Item> tasks = build(K_TASK, W);
    bool tasksScroll = sbW > 0 && ContentHeight(tasks) > g_tasksBottom - g_tasksTop;
    if (tasksScroll) tasks = build(K_TASK, narrowW);
    int tasksContent = ContentHeight(tasks);

    g_scrollTasks = std::max(0, std::min(g_scrollTasks, tasksContent - (g_tasksBottom - g_tasksTop)));
    g_scrollPins  = std::max(0, std::min(g_scrollPins, pinsContent - pinsArea));

    // Indicator sits on the inner edge (away from the screen edge the bar is docked to).
    int nx0 = g_cfg.rightSide ? sbW + sbGap : 0, nx1 = g_cfg.rightSide ? W : W - sbW - sbGap;
    int sbX = g_cfg.rightSide ? 0 : W - sbW;

    g_rows.clear();
    HWND fg = GetForegroundWindow();
    DrawRegion(mdc, W, tasksScroll ? nx0 : 0, tasksScroll ? nx1 : W, tasks, g_tasksTop, g_tasksBottom,
               g_scrollTasks, fg, tasksContent, tasksScroll ? sbX : -1);
    DrawRegion(mdc, W, pinsScroll ? nx0 : 0, pinsScroll ? nx1 : W, pins, g_pinsTop, g_pinsBottom,
               g_scrollPins, fg, pinsContent, pinsScroll ? sbX : -1);

    if (g_cfg.clock) {
        RECT r{0, clockTop, W, clockTop + clockH};
        bool hov = g_hoverKind == K_CLOCK;
        SelectObject(mdc, g_clockFont);
        int saved = g_lineH;
        g_lineH = clockLineH;
        DrawButton(mdc, r, false, g_cfg.outline, HoverFace(g_cfg.clockFace, hov), hov ? g_cfg.hoverText : g_cfg.text, clockLines);
        g_lineH = saved;
        SelectObject(mdc, g_font);
        g_rows.push_back(Row{r, K_CLOCK, 0, r.top, r.bottom});
    }
    if (g_cfg.startButton) {
        RECT r{0, startTop, W, H};
        bool hov = g_hoverKind == K_START;
        DrawStartButton(mdc, r, g_cfg.outline, HoverFace(g_cfg.clockFace, hov), hov ? g_cfg.hoverText : g_cfg.startColor);
        g_rows.push_back(Row{r, K_START, 0, startTop, H});
    }

    BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, oldFont);
    SelectObject(mdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mdc);
    EndPaint(hwnd, &ps);
}

static const Row* HitTest(int x, int y) {
    for (auto& r : g_rows) {
        if (y < r.clipTop || y >= r.clipBottom) continue;
        if (x >= r.r.left && x < r.r.right && y >= r.r.top && y < r.r.bottom) return &r;
    }
    return nullptr;
}

// ---------- audio meters ----------
// Polls every active output device's sessions (WASAPI) and maps each session's peak to the
// window buttons of the same process, or of the same exe (browsers play audio from a child process).

static const GUID kCLSID_MMDeviceEnumerator  = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIID_IMMDeviceEnumerator   = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIID_IAudioSessionManager2 = {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
static const GUID kIID_IAudioSessionControl2 = {0xBFB7FF88, 0x7239, 0x4FC9, {0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D}};
static const GUID kIID_IAudioMeterInformation = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

// IAudioMeterInformation, declared locally because some MinGW headers only forward-declare it.
struct IVbarMeter : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};

struct AudioSess { DWORD pid; std::wstring exe; IVbarMeter* meter; };
static IMMDeviceEnumerator* g_mmEnum = nullptr;
static std::vector<AudioSess> g_sessions;
static DWORD g_sessionsAt = 0;

static std::wstring PidPath(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(p);
    return out;
}

static void AudioRelease() {
    for (auto& a : g_sessions) if (a.meter) a.meter->Release();
    g_sessions.clear();
}

static void AudioRefreshSessions() {
    AudioRelease();
    g_sessionsAt = GetTickCount();
    if (!g_mmEnum && FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                             kIID_IMMDeviceEnumerator, (void**)&g_mmEnum))) {
        g_mmEnum = nullptr;
        return;
    }
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(g_mmEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) || !coll) return;
    UINT nDev = 0;
    coll->GetCount(&nDev);
    for (UINT d = 0; d < nDev; ++d) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(d, &dev)) || !dev) continue;
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(kIID_IAudioSessionManager2, CLSCTX_ALL, nullptr, (void**)&mgr)) && mgr) {
            IAudioSessionEnumerator* en = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&en)) && en) {
                int count = 0;
                en->GetCount(&count);
                for (int j = 0; j < count; ++j) {
                    IAudioSessionControl* ctl = nullptr;
                    if (FAILED(en->GetSession(j, &ctl)) || !ctl) continue;
                    IAudioSessionControl2* ctl2 = nullptr;
                    DWORD pid = 0;
                    if (SUCCEEDED(ctl->QueryInterface(kIID_IAudioSessionControl2, (void**)&ctl2)) && ctl2) {
                        if (ctl2->GetProcessId(&pid) != S_OK) pid = 0;   // skip system sounds / multi-process
                        ctl2->Release();
                    }
                    IVbarMeter* m = nullptr;
                    if (pid && SUCCEEDED(ctl->QueryInterface(kIID_IAudioMeterInformation, (void**)&m)) && m)
                        g_sessions.push_back(AudioSess{pid, PidPath(pid), m});
                    ctl->Release();
                }
                en->Release();
            }
            mgr->Release();
        }
        dev->Release();
    }
    coll->Release();
}

static float ToDisplay(float peak) {
    if (peak <= 0.0f) return 0.0f;
    if (g_cfg.meterDb <= 0) return std::min(1.0f, peak);
    float db = 20.0f * std::log10(peak);
    return std::max(0.0f, std::min(1.0f, 1.0f + db / (float)g_cfg.meterDb));
}

// Returns true if any button's bar changed enough to repaint.
static bool AudioTick() {
    if (g_sessions.empty() || GetTickCount() - g_sessionsAt > 1000) AudioRefreshSessions();

    std::vector<std::pair<AudioSess*, float>> peaks;
    for (auto& a : g_sessions) {
        float p = 0;
        if (SUCCEEDED(a.meter->GetPeakValue(&p)) && p > 0) peaks.push_back({&a, p});
    }

    const float fall = g_cfg.meterFall / 100.0f;
    bool changed = false;
    for (auto& t : g_tasks) {
        float target = 0;
        for (auto& pk : peaks) {
            const AudioSess& a = *pk.first;
            bool direct = a.pid == t.pid;
            bool sameExe = !direct && !t.exe.empty() && _wcsicmp(a.exe.c_str(), t.exe.c_str()) == 0 &&
                           std::none_of(g_tasks.begin(), g_tasks.end(), [&](const Task& o) { return o.pid == a.pid; });
            if (direct || sameExe) target = std::max(target, ToDisplay(pk.second));
        }
        float next = std::max(target, t.level - fall);
        if (next < 0.001f) next = 0;
        if (std::fabs(next - t.level) > 0.002f) changed = true;
        t.level = next;
    }
    return changed;
}

// Blink timer runs only while some window is asking for attention.
static void UpdateBlinkTimer(bool restart) {
    bool any = std::any_of(g_tasks.begin(), g_tasks.end(), [](const Task& t) { return t.flash; });
    if (!any) { KillTimer(g_hwnd, TIMER_BLINK); g_blinkOn = true; g_blinkTicks = 0; return; }
    if (restart) {
        g_blinkOn = true;
        g_blinkTicks = 0;
        SetTimer(g_hwnd, TIMER_BLINK, g_cfg.flashRate, nullptr);
    }
}

static void StartMeterTimer() {
    KillTimer(g_hwnd, TIMER_METER);
    if (g_cfg.meter) SetTimer(g_hwnd, TIMER_METER, 1000 / g_cfg.meterFps, nullptr);
    else for (auto& t : g_tasks) t.level = 0;
}

// ---------- actions ----------

// Brings h to the front without SwitchToThisWindow's slow animated path.
static void ForceForeground(HWND h) {
    if (SetForegroundWindow(h)) return;
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

static void ActivateOrMinimize(HWND h) {
    if (!IsWindow(h)) return;
    if (IsIconic(h)) {
        ShowWindowAsync(h, SW_RESTORE);
        ForceForeground(h);
    } else if (GetForegroundWindow() == h) {
        ShowWindowAsync(h, SW_MINIMIZE);
    } else {
        ForceForeground(h);
    }
}

// Press keys down in order, release in reverse (e.g. {VK_LWIN, 'X'} = Win+X).
static void PressKeys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) {
        --it;
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

static void LaunchPin(const Pin& p) {
    if (!p.aumid.empty()) {
        std::wstring target = L"shell:AppsFolder\\" + p.aumid;
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    std::wstring path = ExpandEnv(p.path), args = ExpandEnv(p.args), dir = ExpandEnv(p.dir);
    ShellExecuteW(nullptr, L"open", path.c_str(), args.empty() ? nullptr : args.c_str(),
                  dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
}

static void Reload() {
    LoadConfig();
    ResetIcons();
    CreateBarFont();
    ShowSystemTaskbars(!g_cfg.hideTaskbar);
    AppBarSetPos();
    RefreshTasks();
    RequestStartPins();
    StartMeterTimer();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Sets key=value in vbar.ini in place (first uncommented match), or appends it.
static void SetIniValue(const std::wstring& key, const std::wstring& value) {
    std::wstring text = DecodeText(ReadFileBytes(g_iniPath));
    std::wstring out, line;
    bool done = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        line = text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        std::wstring t = Trim(line);
        size_t eq = t.find(L'=');
        if (!done && !t.empty() && t[0] != L';' && t[0] != L'#' && eq != std::wstring::npos &&
            _wcsicmp(Trim(t.substr(0, eq)).c_str(), key.c_str()) == 0) {
            bool cr = !line.empty() && line.back() == L'\r';
            line = key + L"=" + value + (cr ? L"\r" : L"");
            done = true;
        }
        out += line;
        if (nl == std::wstring::npos) break;
        out += L'\n';
        start = nl + 1;
    }
    if (!done) {
        if (!out.empty() && out.back() != L'\n') out += L"\r\n";
        out += key + L"=" + value + L"\r\n";
    }
    std::ofstream o(g_iniPath.c_str(), std::ios::binary);
    o << WToUtf8(out);
}

// ---------- start with Windows ----------
// A per-user Run entry (HKCU, no admin needed) pointing at this exe's current location.

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static std::wstring ExePath() {
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    return std::wstring(exe, n);
}

// True only if the entry exists and points at this exe (a stale path from a moved exe counts as off).
static bool AutostartEnabled() {
    wchar_t val[MAX_PATH * 2 + 8];
    DWORD size = sizeof(val), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"vbar", RRF_RT_REG_SZ, &type, val, &size) != ERROR_SUCCESS)
        return false;
    std::wstring v = val;
    v.erase(std::remove(v.begin(), v.end(), L'"'), v.end());
    return _wcsicmp(Trim(v).c_str(), ExePath().c_str()) == 0;
}

static void SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, L"vbar", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"vbar");
    }
    RegCloseKey(k);
}

static void ShowMenu(POINT screenPt) {
    HMENU m = CreatePopupMenu();

    HMENU screens = CreatePopupMenu();
    AppendMenuW(screens, MF_STRING | (g_cfg.monitor == 0 ? MF_CHECKED : 0), IDM_MON_PRIMARY, L"Primary");
    auto mons = Monitors();
    for (size_t i = 0; i < mons.size() && i < 16; ++i) {
        wchar_t label[96];
        wsprintfW(label, L"Display %d  (%dx%d)%s", mons[i].num,
                  mons[i].rc.right - mons[i].rc.left, mons[i].rc.bottom - mons[i].rc.top,
                  mons[i].primary ? L"  primary" : L"");
        AppendMenuW(screens, MF_STRING | (g_cfg.monitor == mons[i].num ? MF_CHECKED : 0),
                    IDM_MON_BASE + i, label);
    }
    AppendMenuW(screens, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(screens, MF_STRING | (!g_cfg.rightSide ? MF_CHECKED : 0), IDM_SIDE_LEFT, L"Left edge");
    AppendMenuW(screens, MF_STRING | (g_cfg.rightSide ? MF_CHECKED : 0), IDM_SIDE_RIGHT, L"Right edge");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)screens, L"Position");
    AppendMenuW(m, MF_STRING | (g_cfg.clock24 ? MF_CHECKED : 0), IDM_CLOCK24, L"24-hour clock");
    bool autostart = AutostartEnabled();
    AppendMenuW(m, MF_STRING | (autostart ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_RELOAD, L"Reload config + Start pins");
    AppendMenuW(m, MF_STRING, IDM_EDIT, L"Edit config");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit vbar");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"vbar " VBAR_VERSION);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == IDM_AUTOSTART) {
        SetAutostart(!autostart);
    } else if (cmd == IDM_CLOCK24) {
        SetIniValue(L"clock24", g_cfg.clock24 ? L"0" : L"1");
        Reload();
    } else if (cmd == IDM_SIDE_LEFT || cmd == IDM_SIDE_RIGHT) {
        SetIniValue(L"side", cmd == IDM_SIDE_RIGHT ? L"right" : L"left");
        Reload();
    } else if (cmd == IDM_MON_PRIMARY) {
        SetIniValue(L"monitor", L"primary");
        Reload();
    } else if (cmd >= IDM_MON_BASE && cmd < IDM_MON_BASE + 16 && cmd - IDM_MON_BASE < (int)mons.size()) {
        SetIniValue(L"monitor", std::to_wstring(mons[cmd - IDM_MON_BASE].num));
        Reload();
    }
    else if (cmd == IDM_RELOAD) Reload();
    else if (cmd == IDM_EDIT) ShellExecuteW(nullptr, L"open", g_iniPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == IDM_EXIT) DestroyWindow(g_hwnd);
}

static void Activate(HWND h) {
    if (!IsWindow(h)) return;
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    ForceForeground(h);
}

// Raise the hovered window right away. Skips work when it's already in front.
static void HoverRaise(int idx) {
    if (idx < 0 || idx >= (int)g_tasks.size()) return;
    HWND h = g_tasks[idx].hwnd;
    if (!IsWindow(h) || GetForegroundWindow() == h) return;
    if (IsIconic(h) && !g_cfg.hoverRestore) return;
    Activate(h);
}

static int TrackMenu(HMENU m, POINT screenPt) {
    KillTimer(g_hwnd, TIMER_HOVER);
    SetForegroundWindow(g_hwnd);                  // required so the menu closes when you click away
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    return cmd;
}

static void ShowTaskMenu(HWND h, POINT screenPt) {
    if (!IsWindow(h)) return;
    bool iconic = IsIconic(h) != 0, zoomed = IsZoomed(h) != 0;
    std::wstring exe = ProcessPath(h);
    UINT noExe = exe.empty() ? MF_GRAYED : 0;

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_T_SWITCH, L"Switch to");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | ((iconic || zoomed) ? 0 : MF_GRAYED), IDM_T_RESTORE, L"Restore");
    AppendMenuW(m, MF_STRING | (iconic ? MF_GRAYED : 0), IDM_T_MINIMIZE, L"Minimize");
    AppendMenuW(m, MF_STRING | (zoomed ? MF_GRAYED : 0), IDM_T_MAXIMIZE, L"Maximize");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | noExe, IDM_T_NEW, L"New instance");
    AppendMenuW(m, MF_STRING | noExe, IDM_T_LOCATION, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_T_CLOSE, L"Close window");
    AppendMenuW(m, MF_STRING, IDM_T_KILL, L"End process");
    SetMenuDefaultItem(m, IDM_T_SWITCH, FALSE);

    switch (TrackMenu(m, screenPt)) {
    case IDM_T_SWITCH:   Activate(h); break;
    case IDM_T_RESTORE:  ShowWindowAsync(h, SW_RESTORE); Activate(h); break;
    case IDM_T_MINIMIZE: ShowWindowAsync(h, SW_MINIMIZE); break;
    case IDM_T_MAXIMIZE: ShowWindowAsync(h, SW_MAXIMIZE); Activate(h); break;
    case IDM_T_NEW:      ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IDM_T_LOCATION: OpenLocation(exe); break;
    case IDM_T_CLOSE:    PostMessageW(h, WM_CLOSE, 0, 0); break;
    case IDM_T_KILL: {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) { TerminateProcess(p, 1); CloseHandle(p); }
        break;
    }
    }
    RefreshTasks();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ShowPinMenu(int idx, POINT screenPt) {
    if (idx < 0 || idx >= PinCount()) return;
    Pin p = PinAt(idx);                           // copy: the list can change while the menu is open
    UINT noPath = p.aumid.empty() ? 0 : MF_GRAYED;

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_P_OPEN, L"Open");
    AppendMenuW(m, MF_STRING | noPath, IDM_P_ADMIN, L"Run as administrator");
    AppendMenuW(m, MF_STRING | noPath, IDM_P_LOCATION, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EDIT, L"Edit config");
    SetMenuDefaultItem(m, IDM_P_OPEN, FALSE);

    switch (TrackMenu(m, screenPt)) {
    case IDM_P_OPEN: LaunchPin(p); break;
    case IDM_P_ADMIN: {
        std::wstring path = ExpandEnv(p.path), args = ExpandEnv(p.args), dir = ExpandEnv(p.dir);
        ShellExecuteW(nullptr, L"runas", path.c_str(), args.empty() ? nullptr : args.c_str(),
                      dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
        break;
    }
    case IDM_P_LOCATION: OpenLocation(Resolve(p.path)); break;
    case IDM_EDIT: ShellExecuteW(nullptr, L"open", g_iniPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    }
}

// ---------- calendar pop-out ----------
// Clicking the clock opens this beside the bar: live time, full date, and a month grid
// in the same bevel style. It never takes focus. It stays open while the mouse is over it
// or the clock, and closes caltimeout seconds after the mouse leaves (re-hovering resets that).
// Wheel changes month, clicking the month name returns to today, clicking the clock closes it.

enum { CAL_PREV = 1, CAL_NEXT = 2, CAL_TITLE = 3, CAL_DAY = 100 };
struct CalHit { RECT r; int id; };
static std::vector<CalHit> g_calHits;

static int DaysIn(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

static int Weekday(int y, int m, int d) {             // 0 = Sunday (Sakamoto)
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

struct CalMetrics { int m, gap, cellW, cellH, bigH, wdH, w, h; };

static CalMetrics CalMeasure(HDC dc) {
    CalMetrics c{};
    c.m = 3;
    c.gap = std::max(1, g_cfg.spacing);
    HGDIOBJ old = SelectObject(dc, g_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE a{}, b{};
    GetTextExtentPoint32W(dc, L"88", 2, &a);
    GetTextExtentPoint32W(dc, L"Mo", 2, &b);
    c.cellW = std::max(a.cx, b.cx) + 2 * g_cfg.pad + 4;
    c.cellH = tm.tmHeight + 2 * g_cfg.vpad + 2;
    c.wdH = tm.tmHeight + 2;
    SelectObject(dc, g_calBigFont);
    GetTextMetricsW(dc, &tm);
    c.bigH = tm.tmHeight + 2 * g_cfg.vpad + 2;
    SelectObject(dc, old);
    c.w = 2 * c.m + 7 * c.cellW + 6 * c.gap;
    c.h = 2 * c.m + c.bigH + c.gap + c.cellH + c.gap + c.cellH + c.gap + c.wdH + c.gap + 6 * c.cellH + 5 * c.gap;
    return c;
}

static void CalCell(HDC dc, RECT r, bool pressed, COLORREF border, COLORREF face, COLORREF txt, const wchar_t* s,
                    UINT align = DT_CENTER) {
    DrawButton(dc, r, pressed, border, face, txt, {});
    int sh = pressed ? 1 : 0;
    RECT tr{r.left + 1 + g_cfg.pad + sh, r.top + 1 + sh, r.right - 1 - g_cfg.pad + sh, r.bottom - 1 + sh};
    SetTextColor(dc, txt);
    DrawTextW(dc, s, -1, &tr, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void CalPaint(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, cr, g_cfg.bg);
    Fill(dc, RECT{0, 0, cr.right, 1}, g_cfg.hi);              // panel bevel: lit top-left edge
    Fill(dc, RECT{0, 0, 1, cr.bottom}, g_cfg.hi);
    g_calHits.clear();

    CalMetrics c = CalMeasure(dc);
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t* DL[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
    static const wchar_t* ML[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                  L"August", L"September", L"October", L"November", L"December"};
    static const wchar_t* WD[] = {L"Su", L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa"};
    int x0 = c.m, x1 = cr.right - c.m, y = c.m;
    wchar_t buf[96];

    // live time
    if (g_cfg.clock24) wsprintfW(buf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    else wsprintfW(buf, L"%d:%02d:%02d %s", (st.wHour + 11) % 12 + 1, st.wMinute, st.wSecond, st.wHour < 12 ? L"AM" : L"PM");
    SelectObject(dc, g_calBigFont);
    CalCell(dc, RECT{x0, y, x1, y + c.bigH}, false, g_cfg.outline, g_cfg.clockFace, g_cfg.hoverText, buf);
    SelectObject(dc, g_font);
    y += c.bigH + c.gap;

    // full date
    wsprintfW(buf, L"%s, %s %d, %d", DL[st.wDayOfWeek], ML[st.wMonth - 1], st.wDay, st.wYear);
    CalCell(dc, RECT{x0, y, x1, y + c.cellH}, false, g_cfg.outline, g_cfg.clockFace, g_cfg.text, buf);
    y += c.cellH + c.gap;

    // month navigation
    RECT rp{x0, y, x0 + c.cellW, y + c.cellH}, rn{x1 - c.cellW, y, x1, y + c.cellH};
    RECT rt{rp.right + c.gap, y, rn.left - c.gap, y + c.cellH};
    CalCell(dc, rp, false, g_cfg.outline, HoverFace(g_cfg.face, g_calHover == CAL_PREV),
            g_calHover == CAL_PREV ? g_cfg.hoverText : g_cfg.text, L"<");
    CalCell(dc, rn, false, g_cfg.outline, HoverFace(g_cfg.face, g_calHover == CAL_NEXT),
            g_calHover == CAL_NEXT ? g_cfg.hoverText : g_cfg.text, L">");
    wsprintfW(buf, L"%s %d", ML[g_calMonth - 1], g_calYear);
    CalCell(dc, rt, false, g_cfg.outline, HoverFace(g_cfg.face, g_calHover == CAL_TITLE),
            g_calHover == CAL_TITLE ? g_cfg.hoverText : g_cfg.text, buf);
    g_calHits.push_back({rp, CAL_PREV});
    g_calHits.push_back({rn, CAL_NEXT});
    g_calHits.push_back({rt, CAL_TITLE});
    y += c.cellH + c.gap;

    // weekday names
    SetTextColor(dc, g_cfg.dim);
    for (int col = 0; col < 7; ++col) {
        int cx = x0 + col * (c.cellW + c.gap);
        RECT r{cx, y, cx + c.cellW, y + c.wdH};
        DrawTextW(dc, WD[(col + g_cfg.weekStart) % 7], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    y += c.wdH + c.gap;

    // 6-week grid
    int first = Weekday(g_calYear, g_calMonth, 1);
    int offset = (first - g_cfg.weekStart + 7) % 7;
    int pm = g_calMonth == 1 ? 12 : g_calMonth - 1, py = g_calMonth == 1 ? g_calYear - 1 : g_calYear;
    int nm = g_calMonth == 12 ? 1 : g_calMonth + 1, ny = g_calMonth == 12 ? g_calYear + 1 : g_calYear;
    int dim = DaysIn(g_calYear, g_calMonth), dimPrev = DaysIn(py, pm);
    for (int i = 0; i < 42; ++i) {
        int d = i - offset + 1, cy = g_calYear, cm = g_calMonth;
        bool inMonth = true;
        if (d < 1) { d += dimPrev; cy = py; cm = pm; inMonth = false; }
        else if (d > dim) { d -= dim; cy = ny; cm = nm; inMonth = false; }
        bool today = cy == st.wYear && cm == st.wMonth && d == st.wDay;
        bool hov = g_calHover == CAL_DAY + i;
        int cx = x0 + (i % 7) * (c.cellW + c.gap), ry = y + (i / 7) * (c.cellH + c.gap);
        RECT r{cx, ry, cx + c.cellW, ry + c.cellH};
        wsprintfW(buf, L"%d", d);
        COLORREF txt = today ? g_cfg.activeText : hov ? g_cfg.hoverText : inMonth ? g_cfg.text : g_cfg.dim;
        CalCell(dc, r, today, today ? g_cfg.activeBorder : g_cfg.outline,
                HoverFace(today ? g_cfg.activeFace : g_cfg.face, hov), txt, buf);
        g_calHits.push_back({r, CAL_DAY + i});
    }

    BitBlt(hdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static void CalHide() {
    if (!g_calHwnd || !IsWindowVisible(g_calHwnd)) return;
    KillTimer(g_calHwnd, TIMER_CAL);
    ShowWindow(g_calHwnd, SW_HIDE);
    g_calClosedAt = GetTickCount();
}

static void CalShiftMonth(int delta) {
    int idx = g_calYear * 12 + (g_calMonth - 1) + delta;
    g_calYear = idx / 12;
    g_calMonth = idx % 12 + 1;
    InvalidateRect(g_calHwnd, nullptr, FALSE);
}

static void CalToday() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    g_calYear = st.wYear;
    g_calMonth = st.wMonth;
}

static LRESULT CALLBACK CalProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: CalPaint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (wp == TIMER_CAL) {
            POINT pt;
            GetCursorPos(&pt);
            RECT cal, bar;
            GetWindowRect(h, &cal);
            GetWindowRect(g_hwnd, &bar);
            bool over = PtInRect(&cal, pt) != 0;
            for (auto& rw : g_rows)
                if (rw.kind == K_CLOCK) {
                    RECT cr = rw.r;
                    OffsetRect(&cr, bar.left, bar.top);
                    if (PtInRect(&cr, pt)) over = true;
                }
            DWORD now = GetTickCount();
            if (over) g_calLastHover = now;
            else if (now - g_calLastHover >= (DWORD)g_cfg.calTimeout * 1000) { CalHide(); return 0; }
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (st.wSecond != g_calLastSecond) { g_calLastSecond = st.wSecond; InvalidateRect(h, nullptr, FALSE); }
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) CalHide();
        else if (wp == VK_LEFT || wp == VK_PRIOR) CalShiftMonth(-1);
        else if (wp == VK_RIGHT || wp == VK_NEXT) CalShiftMonth(1);
        else if (wp == VK_HOME) { CalToday(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        CalShiftMonth(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        int hit = 0;
        for (auto& c : g_calHits) if (PtInRect(&c.r, pt)) { hit = c.id; break; }
        if (hit != g_calHover) { g_calHover = hit; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_calHover) { g_calHover = 0; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        for (auto& c : g_calHits) {
            if (!PtInRect(&c.r, pt)) continue;
            if (c.id == CAL_PREV) CalShiftMonth(-1);
            else if (c.id == CAL_NEXT) CalShiftMonth(1);
            else if (c.id == CAL_TITLE) { CalToday(); InvalidateRect(h, nullptr, FALSE); }
            break;
        }
        return 0;
    }
    case WM_RBUTTONUP:
        CalHide();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void CalToggle() {
    if (g_calHwnd && IsWindowVisible(g_calHwnd)) { CalHide(); return; }
    if (!g_calHwnd) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CalProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"VBarCal";
        RegisterClassExW(&wc);
        g_calHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"VBarCal", L"vbar calendar", WS_POPUP,
                                    0, 0, 10, 10, g_hwnd, nullptr, wc.hInstance, nullptr);
        if (!g_calHwnd) return;
    }
    CalToday();
    g_calHover = 0;

    HDC sdc = GetDC(nullptr);
    CalMetrics c = CalMeasure(sdc);
    ReleaseDC(nullptr, sdc);

    // Beside the bar on its inner side, bottom-aligned with the clock, kept on the bar's monitor.
    RECT bar;
    GetWindowRect(g_hwnd, &bar);
    int clockBottom = bar.bottom;
    for (auto& rw : g_rows) if (rw.kind == K_CLOCK) clockBottom = bar.top + rw.r.bottom;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = g_cfg.rightSide ? bar.left - c.w - 2 : bar.right + 2;
    int y = std::max((int)mi.rcMonitor.top, std::min(clockBottom, (int)mi.rcMonitor.bottom) - c.h);
    SetWindowPos(g_calHwnd, HWND_TOPMOST, x, y, c.w, c.h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    g_calLastHover = GetTickCount();
    SetTimer(g_calHwnd, TIMER_CAL, 200, nullptr);
    InvalidateRect(g_calHwnd, nullptr, FALSE);
}

// ---------- window proc ----------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_shellMsg && g_shellMsg) {
        HWND h = (HWND)lp;
        bool newFlash = false;
        if (wp == HSHELL_FLASH) {
            for (auto& t : g_tasks) if (t.hwnd == h && !t.flash) { t.flash = true; newFlash = true; }
        } else if ((wp & 0x7FFF) == HSHELL_WINDOWACTIVATED) {
            for (auto& t : g_tasks) if (t.hwnd == h) t.flash = false;
        }
        RefreshTasks();
        UpdateBlinkTimer(newFlash);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {     // explorer restarted
        AppBarRemove();
        AppBarRegister();
        ShowSystemTaskbars(!g_cfg.hideTaskbar);
        return 0;
    }

    switch (msg) {
    case WM_STARTPINS: {
        StartPinsResult* res = (StartPinsResult*)lp;
        if (res->gen == g_startPinsGen && g_cfg.startPins) {
            g_startPins.clear();
            for (auto& p : res->pins) {
                bool dup = std::any_of(g_cfg.pins.begin(), g_cfg.pins.end(), [&](const Pin& c) {
                    return _wcsicmp(c.name.c_str(), p.name.c_str()) == 0;
                });
                if (!dup) g_startPins.push_back(p);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete res;
        return 0;
    }

    case WM_APPBAR:
        if (wp == ABN_POSCHANGED) AppBarSetPos();
        else if (wp == ABN_FULLSCREENAPP)
            SetWindowPos(hwnd, lp ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_TIMER:
        if (wp == TIMER_SCAN) {
            if (RefreshTasks()) InvalidateRect(hwnd, nullptr, FALSE);
            UpdateBlinkTimer(false);
            if (g_cfg.hideTaskbar) {
                HWND t = FindWindowW(L"Shell_TrayWnd", nullptr);
                if (t && IsWindowVisible(t)) ShowSystemTaskbars(false);
            }
        } else if (wp == TIMER_HOVER) {
            KillTimer(hwnd, TIMER_HOVER);
            if (g_hoverKind == K_TASK) HoverRaise(g_hoverIdx);
        } else if (wp == TIMER_BLINK) {
            UpdateBlinkTimer(false);
            if (std::any_of(g_tasks.begin(), g_tasks.end(), [](const Task& t) { return t.flash; })) {
                ++g_blinkTicks;
                if (g_cfg.flashCount > 0 && g_blinkTicks >= g_cfg.flashCount * 2) {
                    g_blinkOn = true;                    // done blinking: stay lit until focused
                    KillTimer(hwnd, TIMER_BLINK);
                } else {
                    g_blinkOn = g_cfg.flashCount == 0 ? true : !g_blinkOn;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (wp == TIMER_METER) {
            if (AudioTick()) InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == TIMER_CLOCK && g_cfg.clock) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (st.wMinute != g_lastMinute) {
                g_lastMinute = st.wMinute;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (!g_tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g_tracking = true;
        }
        const Row* r = HitTest((short)LOWORD(lp), (short)HIWORD(lp));
        int k = r ? r->kind : K_NONE, i = r ? r->idx : -1;
        if (k != g_hoverKind || i != g_hoverIdx) {
            g_hoverKind = k;
            g_hoverIdx = i;
            KillTimer(hwnd, TIMER_HOVER);
            InvalidateRect(hwnd, nullptr, FALSE);
            if (k == K_TASK && g_cfg.hoverFocus) {
                if (g_cfg.hoverDelay == 0) HoverRaise(i);
                else SetTimer(hwnd, TIMER_HOVER, g_cfg.hoverDelay, nullptr);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        KillTimer(hwnd, TIMER_HOVER);
        g_tracking = false;
        g_hoverKind = K_NONE;
        g_hoverIdx = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};       // screen coords
        ScreenToClient(hwnd, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * (g_lineH + g_cfg.vpad) * 3;
        if (pt.y >= g_pinsTop && pt.y < g_pinsBottom) g_scrollPins -= delta;
        else g_scrollTasks -= delta;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        KillTimer(hwnd, TIMER_HOVER);                 // a click wins over a pending hover-raise
        const Row* r = HitTest((short)LOWORD(lp), (short)HIWORD(lp));
        if (!r) return 0;
        if (r->kind == K_START) { PressKeys({VK_LWIN}); return 0; }
        if (r->kind == K_CLOCK) { CalToggle(); return 0; }
        if (r->kind == K_PIN && r->idx < PinCount()) LaunchPin(PinAt(r->idx));
        else if (r->kind == K_TASK && r->idx < (int)g_tasks.size()) ActivateOrMinimize(g_tasks[r->idx].hwnd);
        RefreshTasks();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MBUTTONUP: {
        const Row* r = HitTest((short)LOWORD(lp), (short)HIWORD(lp));
        if (r && r->kind == K_TASK && r->idx < (int)g_tasks.size())
            PostMessageW(g_tasks[r->idx].hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        const Row* r = HitTest(pt.x, pt.y);
        int kind = r ? r->kind : K_NONE, idx = r ? r->idx : -1;
        if (kind == K_START) { PressKeys({VK_LWIN, 'X'}); return 0; }
        ClientToScreen(hwnd, &pt);
        if (kind == K_PIN && idx < PinCount()) ShowPinMenu(idx, pt);
        else if (kind == K_TASK && idx < (int)g_tasks.size()) ShowTaskMenu(g_tasks[idx].hwnd, pt);
        else ShowMenu(pt);
        return 0;
    }

    case WM_DISPLAYCHANGE:
        AppBarSetPos();
        return 0;

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_SCAN);
        KillTimer(hwnd, TIMER_CLOCK);
        KillTimer(hwnd, TIMER_HOVER);
        KillTimer(hwnd, TIMER_METER);
        KillTimer(hwnd, TIMER_BLINK);
        AudioRelease();
        if (g_mmEnum) { g_mmEnum->Release(); g_mmEnum = nullptr; }
        DeregisterShellHookWindow(hwnd);
        AppBarRemove();
        ShowSystemTaskbars(true);
        if (!g_loadedFontFile.empty()) RemoveFontResourceExW(g_loadedFontFile.c_str(), FR_PRIVATE, nullptr);
        if (g_font) DeleteObject(g_font);
        if (g_clockFont) DeleteObject(g_clockFont);
        if (g_calBigFont) DeleteObject(g_calBigFont);
        if (g_calHwnd) DestroyWindow(g_calHwnd);
        g_pinIcons.clear();
        g_tasks.clear();
        if (g_iconDC) DeleteDC(g_iconDC);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------- entry ----------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"vbar_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = exe;
    g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L"\\/"));
    g_iniPath = g_exeDir + L"\\vbar.ini";

    LoadConfig();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VBarWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"VBarWnd", L"vbar",
                             WS_POPUP, 0, 0, g_cfg.width, 100, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    CreateBarFont();
    g_shellMsg = RegisterWindowMessageW(L"SHELLHOOK");
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    RegisterShellHookWindow(g_hwnd);
    AppBarRegister();
    if (g_cfg.hideTaskbar) ShowSystemTaskbars(false);
    RefreshTasks();
    RequestStartPins();

    SetTimer(g_hwnd, TIMER_SCAN, 500, nullptr);
    SetTimer(g_hwnd, TIMER_CLOCK, 1000, nullptr);
    StartMeterTimer();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    CoUninitialize();
    return 0;
}
