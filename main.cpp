// VideoWallpaper - Lightweight live video wallpaper for Windows
// Supports both legacy WorkerW trick (Win 7-10) and Win 11 24H2+ (child of Progman)
// Per-monitor support: one window + one MFPlay player per monitor.
// Usage: Place config.txt next to .exe with the absolute path to a video file.
// Press Ctrl+Alt+Q to quit.

#include <windows.h>
#include <mfplay.h>
#include <mfapi.h>
#include <mfidl.h>
#include <shlwapi.h>
#include <propvarutil.h>

#include <fstream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
namespace { struct MonitorWallpaper; }
static void ShutdownAllMonitors();

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
namespace
{
    HANDLE g_mutex = nullptr;
    HWND g_msgWindow = nullptr;         // Hidden message-only window for hotkey
    bool g_debugEnabled = false;
    std::ofstream g_logFile;
    std::wstring g_videoPath;
    HINSTANCE g_inst = nullptr;
    const wchar_t* g_wpClass = L"VideoWallpaperClass";

    // Per-monitor data
    struct MonitorWallpaper
    {
        HWND window = nullptr;
        IMFPMediaPlayer* player = nullptr;
        RECT rect = {};
    };
    std::vector<MonitorWallpaper> g_monitors;

    // Desktop detection result
    struct DesktopWindows
    {
        HWND progman       = nullptr;
        HWND shellDefView  = nullptr;
        HWND workerW       = nullptr;
        bool shellOnProgman = false;
    };
    DesktopWindows g_desktop;

    // ---------------------------------------------------------------------------
    // Logging (only active when debug.flag file exists next to .exe)
    // ---------------------------------------------------------------------------
    std::wstring GetExeDir()
    {
        wchar_t dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        PathRemoveFileSpecW(dir);
        return dir;
    }

    bool IsDebugEnabled()
    {
        std::wstring flagPath = GetExeDir() + L"\\debug.flag";
        return GetFileAttributesW(flagPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void Log(const std::wstring& msg)
    {
        if (!g_debugEnabled) return;
        if (!g_logFile.is_open())
        {
            std::wstring logPath = GetExeDir() + L"\\debug.log";
            g_logFile.open(logPath.c_str());
        }
        g_logFile << std::string(msg.begin(), msg.end()) << '\n';
    }

    void CloseLog()
    {
        if (g_logFile.is_open()) g_logFile.close();
    }

    // ---------------------------------------------------------------------------
    // Config
    // ---------------------------------------------------------------------------
    std::wstring Trim(const std::wstring& s)
    {
        const wchar_t* ws = L" \t\r\n\"";
        auto a = s.find_first_not_of(ws);
        if (a == std::wstring::npos) return {};
        auto b = s.find_last_not_of(ws);
        return s.substr(a, b - a + 1);
    }

    std::wstring ReadVideoPath()
    {
        std::wstring cfgPath = GetExeDir() + L"\\config.txt";
        std::wifstream f(cfgPath.c_str());
        std::wstring p;
        std::getline(f, p);
        return Trim(p);
    }

    // ---------------------------------------------------------------------------
    // Desktop window detection (Win10 legacy AND Win11 24H2+)
    // ---------------------------------------------------------------------------
    struct LegacySearch { HWND workerWithShell = nullptr; HWND workerWithout = nullptr; };

    BOOL CALLBACK LegacyEnumProc(HWND hwnd, LPARAM lParam)
    {
        auto* s = reinterpret_cast<LegacySearch*>(lParam);
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        if (lstrcmpW(cls, L"WorkerW") != 0) return TRUE;
        HWND sv = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (sv) s->workerWithShell = hwnd;
        else if (!s->workerWithout) s->workerWithout = hwnd;
        return TRUE;
    }

    struct ProgmanChildren { HWND shellDefView = nullptr; HWND workerW = nullptr; };

    BOOL CALLBACK ProgmanChildProc(HWND hwnd, LPARAM lParam)
    {
        auto* s = reinterpret_cast<ProgmanChildren*>(lParam);
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        if (lstrcmpW(cls, L"SHELLDLL_DefView") == 0 && !s->shellDefView) s->shellDefView = hwnd;
        if (lstrcmpW(cls, L"WorkerW") == 0 && !s->workerW) s->workerW = hwnd;
        return TRUE;
    }

    DesktopWindows FindDesktopWindows()
    {
        DesktopWindows dw;
        dw.progman = FindWindowW(L"Progman", nullptr);
        if (!dw.progman) { Log(L"ERROR: Progman not found!"); return dw; }

        DWORD_PTR r = 0;
        SendMessageTimeoutW(dw.progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &r);
        Log(L"Sent 0x052C to Progman.");

        HWND directShell = FindWindowExW(dw.progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (directShell)
        {
            Log(L"Win11 24H2+ mode.");
            dw.shellDefView = directShell;
            dw.shellOnProgman = true;
            ProgmanChildren pc;
            EnumChildWindows(dw.progman, ProgmanChildProc, reinterpret_cast<LPARAM>(&pc));
            dw.workerW = pc.workerW;
            return dw;
        }

        Log(L"Legacy WorkerW mode.");
        for (int i = 0; i < 20; ++i)
        {
            LegacySearch ls;
            EnumWindows(LegacyEnumProc, reinterpret_cast<LPARAM>(&ls));
            if (ls.workerWithShell && ls.workerWithout)
            {
                dw.workerW = ls.workerWithout;
                dw.shellDefView = FindWindowExW(ls.workerWithShell, nullptr, L"SHELLDLL_DefView", nullptr);
                return dw;
            }
            Sleep(100);
        }
        Log(L"Legacy mode: timeout.");
        return dw;
    }

    // ---------------------------------------------------------------------------
    // Monitor enumeration
    // ---------------------------------------------------------------------------
    BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT lpRect, LPARAM lParam)
    {
        auto* rects = reinterpret_cast<std::vector<RECT>*>(lParam);
        rects->push_back(*lpRect);
        return TRUE;
    }

    std::vector<RECT> EnumerateMonitors()
    {
        std::vector<RECT> rects;
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&rects));
        Log(L"Found " + std::to_wstring(rects.size()) + L" monitor(s).");
        for (size_t i = 0; i < rects.size(); ++i)
        {
            auto& r = rects[i];
            Log(L"  Monitor " + std::to_wstring(i) + L": "
                + std::to_wstring(r.right - r.left) + L"x" + std::to_wstring(r.bottom - r.top)
                + L" at (" + std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L")");
        }
        return rects;
    }

    // ---------------------------------------------------------------------------
    // MFPlay callback — finds its player in g_monitors by HWND
    // ---------------------------------------------------------------------------
    class MediaPlayerCallback final : public IMFPMediaPlayerCallback
    {
    public:
        explicit MediaPlayerCallback(int monitorIndex) : monIdx_(monitorIndex) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback))
            {
                *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
                AddRef(); return S_OK;
            }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&rc_); }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG c = InterlockedDecrement(&rc_);
            if (!c) delete this;
            return c;
        }

        void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* hdr) override
        {
            if (!hdr) return;
            if (monIdx_ < 0 || monIdx_ >= static_cast<int>(g_monitors.size())) return;
            auto* player = g_monitors[monIdx_].player;
            if (!player) return;

            switch (hdr->eEventType)
            {
            case MFP_EVENT_TYPE_MEDIAITEM_SET:
                Log(L"Monitor " + std::to_wstring(monIdx_) + L": Playing.");
                player->Play();
                player->UpdateVideo();
                break;
            case MFP_EVENT_TYPE_PLAYBACK_ENDED:
                Log(L"Monitor " + std::to_wstring(monIdx_) + L": Looping.");
                {
                    PROPVARIANT pos; PropVariantInit(&pos);
                    pos.vt = VT_I8; pos.hVal.QuadPart = 0;
                    player->SetPosition(MFP_POSITIONTYPE_100NS, &pos);
                    PropVariantClear(&pos);
                    player->Play();
                }
                break;
            default: break;
            }

            if (FAILED(hdr->hrEvent))
                Log(L"Monitor " + std::to_wstring(monIdx_) + L" MFP Error: "
                    + std::to_wstring(static_cast<long>(hdr->hrEvent)));
        }
    private:
        ~MediaPlayerCallback() = default;
        long rc_ = 1;
        int monIdx_ = 0;
    };
}

// ---------------------------------------------------------------------------
// Wallpaper window procedure (per-monitor windows)
// ---------------------------------------------------------------------------
LRESULT CALLBACK WpWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        for (auto& m : g_monitors)
            if (m.window == hwnd && m.player) m.player->UpdateVideo();
        return 0;
    }
    case WM_SIZE:
        for (auto& m : g_monitors)
            if (m.window == hwnd && m.player) m.player->UpdateVideo();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// System tray
// ---------------------------------------------------------------------------
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_QUIT 1001

namespace
{
    NOTIFYICONDATAW g_nid = {};

    void AddTrayIcon(HWND hwnd)
    {
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        // Load custom icon from resource (ID 101 defined in app.rc), fallback to default
        g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy(g_nid.szTip, L"VideoWallpaper");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
    }

    void RemoveTrayIcon()
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
    }

    void ShowTrayMenu(HWND hwnd)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit VideoWallpaper");

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }
}

// ---------------------------------------------------------------------------
// Hidden message window procedure (hotkey + display change + tray)
// ---------------------------------------------------------------------------
LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT, 'Q');
        AddTrayIcon(hwnd);
        return 0;
    case WM_HOTKEY:
        if (wp == 1) DestroyWindow(hwnd);
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            ShowTrayMenu(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_TRAY_QUIT)
            DestroyWindow(hwnd);
        return 0;
    case WM_DISPLAYCHANGE:
        Log(L"Display change detected.");
        {
            auto rects = EnumerateMonitors();
            for (size_t i = 0; i < g_monitors.size() && i < rects.size(); ++i)
            {
                auto& r = rects[i];
                auto& m = g_monitors[i];
                m.rect = r;
                HWND parent = GetParent(m.window);
                POINT pt = { r.left, r.top };
                if (parent) MapWindowPoints(nullptr, parent, &pt, 1);
                SetWindowPos(m.window, nullptr, pt.x, pt.y,
                             r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                if (m.player) m.player->UpdateVideo();
            }
        }
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        UnregisterHotKey(hwnd, 1);
        ShutdownAllMonitors();
        MFShutdown();
        CoUninitialize();
        CloseLog();
        if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Shutdown all per-monitor players
// ---------------------------------------------------------------------------
static void ShutdownAllMonitors()
{
    for (auto& m : g_monitors)
    {
        if (m.player) { m.player->Shutdown(); m.player->Release(); m.player = nullptr; }
        if (m.window) { DestroyWindow(m.window); m.window = nullptr; }
    }
    g_monitors.clear();
}

// ---------------------------------------------------------------------------
// Create wallpaper windows for all monitors
// ---------------------------------------------------------------------------
static bool CreateMonitorWallpapers(const DesktopWindows& dw)
{
    auto rects = EnumerateMonitors();
    if (rects.empty()) return false;

    HWND insertAfter = dw.shellDefView; // First window goes below ShellDefView

    for (size_t i = 0; i < rects.size(); ++i)
    {
        auto r = rects[i]; // copy — we'll modify for coordinate conversion
        int w = r.right - r.left;
        int h = r.bottom - r.top;

        MonitorWallpaper mw;
        mw.rect = rects[i]; // keep original screen coords

        if (dw.shellOnProgman)
        {
            // Win11 24H2+: create popup, reparent into Progman
            mw.window = CreateWindowExW(
                0, g_wpClass, L"",
                WS_POPUP | WS_VISIBLE,
                r.left, r.top, w, h,
                nullptr, nullptr, g_inst, nullptr
            );
            if (!mw.window) { Log(L"Failed to create window for monitor " + std::to_wstring(i)); continue; }

            SetParent(mw.window, dw.progman);
            LONG style = GetWindowLong(mw.window, GWL_STYLE);
            style = (style & ~WS_POPUP) | WS_CHILD;
            SetWindowLong(mw.window, GWL_STYLE, style);

            // Convert screen coordinates to Progman client coordinates
            POINT pt = { r.left, r.top };
            MapWindowPoints(nullptr, dw.progman, &pt, 1);

            Log(L"Monitor " + std::to_wstring(i) + L": screen(" + std::to_wstring(r.left) + L","
                + std::to_wstring(r.top) + L") -> client(" + std::to_wstring(pt.x) + L"," + std::to_wstring(pt.y) + L")");

            if (insertAfter)
                SetWindowPos(mw.window, insertAfter, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            else
                SetWindowPos(mw.window, HWND_BOTTOM, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

            // Next monitor window goes after this one in Z-order
            insertAfter = mw.window;
        }
        else
        {
            // Legacy: child of WorkerW — also need client coord conversion
            HWND host = dw.workerW ? dw.workerW : dw.progman;

            POINT pt = { r.left, r.top };
            MapWindowPoints(nullptr, host, &pt, 1);

            mw.window = CreateWindowExW(
                0, g_wpClass, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                pt.x, pt.y, w, h,
                host, nullptr, g_inst, nullptr
            );
            if (!mw.window) { Log(L"Failed to create window for monitor " + std::to_wstring(i)); continue; }
            SetWindowPos(mw.window, HWND_BOTTOM, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        g_monitors.push_back(mw);
        Log(L"Created window for monitor " + std::to_wstring(i) + L": "
            + std::to_wstring(w) + L"x" + std::to_wstring(h));
    }

    // Hide static wallpaper
    if (dw.shellOnProgman && dw.workerW)
    {
        ShowWindow(dw.workerW, SW_HIDE);
        Log(L"Hid static wallpaper WorkerW.");
    }

    return !g_monitors.empty();
}

// ---------------------------------------------------------------------------
// Create MFPlay player for each monitor window
// ---------------------------------------------------------------------------
static bool CreatePlayers()
{
    for (size_t i = 0; i < g_monitors.size(); ++i)
    {
        auto& m = g_monitors[i];
        auto* cb = new MediaPlayerCallback(static_cast<int>(i));
        HRESULT hr = MFPCreateMediaPlayer(
            g_videoPath.c_str(), TRUE, 0, cb, m.window, &m.player
        );
        cb->Release();

        if (FAILED(hr) || !m.player)
        {
            Log(L"MFPCreateMediaPlayer FAILED for monitor " + std::to_wstring(i)
                + L" hr=" + std::to_wstring(static_cast<long>(hr)));
            return false;
        }

        // Strip audio — wallpapers don't need sound, saves decoding overhead
        m.player->SetMute(TRUE);

        ShowWindow(m.window, SW_SHOW);
        UpdateWindow(m.window);
        Log(L"Player created for monitor " + std::to_wstring(i));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    g_inst = inst;

    // --- Single instance guard ---
    g_mutex = CreateMutexW(nullptr, TRUE, L"Global\\VideoWallpaperMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"VideoWallpaper is already running.", L"VideoWallpaper", MB_ICONINFORMATION);
        return 0;
    }

    // --- Init debug logging ---
    g_debugEnabled = IsDebugEnabled();
    if (g_debugEnabled) Log(L"Debug logging enabled.");

    // --- Read and validate config ---
    g_videoPath = ReadVideoPath();
    if (g_videoPath.empty())
    {
        MessageBoxW(nullptr, L"config.txt is empty or missing.\n\nPlace a video file path in config.txt next to VideoWallpaper.exe.",
                     L"VideoWallpaper", MB_ICONERROR);
        return 1;
    }
    if (GetFileAttributesW(g_videoPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::wstring msg = L"Video file not found:\n" + g_videoPath;
        MessageBoxW(nullptr, msg.c_str(), L"VideoWallpaper", MB_ICONERROR);
        return 1;
    }
    Log(L"Video path: " + g_videoPath);

    // --- COM & Media Foundation ---
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }

    // --- Register window classes ---
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WpWndProc;
    wc.hInstance      = inst;
    wc.lpszClassName  = g_wpClass;
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    const wchar_t* msgClass = L"VideoWallpaperMsgClass";
    WNDCLASSW mc{};
    mc.lpfnWndProc   = MsgWndProc;
    mc.hInstance      = inst;
    mc.lpszClassName  = msgClass;
    RegisterClassW(&mc);

    // --- Find desktop windows ---
    g_desktop = FindDesktopWindows();
    if (!g_desktop.progman)
    {
        MessageBoxW(nullptr, L"Could not find the desktop window (Progman).", L"VideoWallpaper", MB_ICONERROR);
        MFShutdown(); CoUninitialize();
        return 1;
    }

    // --- Show host for legacy mode ---
    if (!g_desktop.shellOnProgman)
    {
        HWND host = g_desktop.workerW ? g_desktop.workerW : g_desktop.progman;
        ShowWindow(host, SW_SHOWNA);
    }

    // --- Create per-monitor wallpaper windows ---
    if (!CreateMonitorWallpapers(g_desktop))
    {
        MessageBoxW(nullptr, L"Failed to create wallpaper windows.", L"VideoWallpaper", MB_ICONERROR);
        MFShutdown(); CoUninitialize();
        return 1;
    }

    // --- Create a player per monitor ---
    if (!CreatePlayers())
    {
        std::wstring msg = L"Failed to create media player.\n\nFile: " + g_videoPath;
        MessageBoxW(nullptr, msg.c_str(), L"VideoWallpaper", MB_ICONERROR);
        ShutdownAllMonitors();
        MFShutdown(); CoUninitialize();
        return 1;
    }

    // --- Hidden message window for hotkey & display change ---
    g_msgWindow = CreateWindowExW(
        0, msgClass, L"", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, inst, nullptr
    );

    // --- Message loop ---
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}