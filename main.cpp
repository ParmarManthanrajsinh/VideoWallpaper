// VideoWallpaper - Lightweight live video wallpaper for Windows
// Supports both legacy WorkerW trick (Win 7-10) and Win 11 24H2+ (child of Progman)
// Per-monitor support: one window + one MFPlay player per monitor.
// Usage: Place config.txt next to .exe with the absolute path to a video file.
// Press Ctrl+Alt+Q to quit.

#include <windows.h>
#include <psapi.h>
#include <mfplay.h>
#include <mfapi.h>
#include <mfidl.h>
#include <shlwapi.h>
#include <propvarutil.h>
#include <commdlg.h>

#include <cstdint>
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
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#endif


/** Undocumented Progman message to spawn a WorkerW behind the desktop icons. */
constexpr UINT WM_SPAWN_WORKERW = 0x052C;

/** Pre-seek threshold: begin looping when within 500ms (in 100ns units) of end. */
constexpr LONGLONG PreSeekThreshold100ns = 5000000LL;

/** Timer ID for the periodic update tick. */
constexpr UINT_PTR TimerIdUpdate = 100;

/** Timer interval in milliseconds. */
constexpr UINT TimerIntervalMs = 500;

/** Maximum number of boot retries waiting for desktop. */
constexpr int32_t MaxDesktopRetries = 30;

/** Size of class name buffers for GetClassNameW calls. */
constexpr int32_t ClassNameBufferSize = 64;

namespace
{
    void ShutdownAllMonitors();
    void ChangeVideo();
    bool IsAutoStartEnabled();
    void SetAutoStart(bool bEnable);

    HANDLE GMutex = nullptr;
    HWND GMsgWindow = nullptr;

    bool GbDebugEnabled = false;
    std::ofstream GLogFile;
    std::wstring GVideoPath;
    bool GbPaused = false;
    bool GbAutoPausedByFullscreen = false;
    bool GbMuted = true;
    HINSTANCE GInstance = nullptr;
    const wchar_t* GWallpaperClassName = L"VideoWallpaperClass";

    struct FMonitorWallpaper
    {
        HWND Window = nullptr;
        IMFPMediaPlayer* Player = nullptr;
        RECT Rect = {};
        LONGLONG Duration = 0;

    };
    std::vector<FMonitorWallpaper> GMonitors;

    struct FDesktopWindows
    {
        HWND Progman = nullptr;
        HWND ShellDefView = nullptr;
        HWND WorkerW = nullptr;
        bool bShellOnProgman = false;
    };
    FDesktopWindows GDesktop;

    std::wstring GetExeDir()
    {
        wchar_t Dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, Dir, MAX_PATH);
        PathRemoveFileSpecW(Dir);
        return Dir;
    }

    bool IsDebugFlagPresent()
    {
        std::wstring FlagPath = GetExeDir() + L"\\debug.flag";
        return GetFileAttributesW(FlagPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void Log(const std::wstring& Message)
    {
        if (!GbDebugEnabled) return;
        if (!GLogFile.is_open())
        {
            std::wstring LogPath = GetExeDir() + L"\\debug.log";
            GLogFile.open(LogPath.c_str());
        }
        int32_t SizeNeeded = WideCharToMultiByte
        (
            CP_UTF8,
            0,
            Message.c_str(),
            static_cast<int>(Message.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (SizeNeeded > 0)
        {
            std::string Utf8(SizeNeeded, '\0');
            WideCharToMultiByte
            (
                CP_UTF8,
                0,
                Message.c_str(),
                static_cast<int>(Message.size()),
                &Utf8[0],
                SizeNeeded,
                nullptr,
                nullptr
            );
            GLogFile << Utf8 << '\n';
        }
        GLogFile.flush();
    }

    void CloseLog()
    {
        if (GLogFile.is_open()) GLogFile.close();
    }

    std::wstring TrimString(const std::wstring& InString)
    {
        const wchar_t* Whitespace = L" \t\r\n\"";
        auto Start = InString.find_first_not_of(Whitespace);
        if (Start == std::wstring::npos) return {};
        auto End = InString.find_last_not_of(Whitespace);
        return InString.substr(Start, End - Start + 1);
    }

    std::wstring ReadVideoPath()
    {
        std::wstring ConfigPath = GetExeDir() + L"\\config.txt";
        std::wifstream File(ConfigPath.c_str());
        std::wstring Path;
        std::getline(File, Path);
        return TrimString(Path);
    }

    struct FLegacySearch
    {
        HWND WorkerWithShell = nullptr;
        HWND WorkerWithout = nullptr;
    };

    BOOL CALLBACK LegacyEnumProc(HWND Hwnd, LPARAM LParam)
    {
        auto* Search = reinterpret_cast<FLegacySearch*>(LParam);
        wchar_t ClassName[ClassNameBufferSize] = {};
        GetClassNameW(Hwnd, ClassName, ClassNameBufferSize);
        if (lstrcmpW(ClassName, L"WorkerW") != 0) return TRUE;
        HWND ShellView = FindWindowExW(Hwnd, nullptr, L"SHELLDLL_DefView", nullptr);

        if (ShellView) 
            Search->WorkerWithShell = Hwnd;
        else if (!Search->WorkerWithout) 
            Search->WorkerWithout = Hwnd;

        return TRUE;
    }

    struct FProgmanChildren
    {
        HWND ShellDefView = nullptr;
        HWND WorkerW = nullptr;
    };

    BOOL CALLBACK ProgmanChildProc(HWND Hwnd, LPARAM LParam)
    {
        auto* Children = reinterpret_cast<FProgmanChildren*>(LParam);
        wchar_t ClassName[ClassNameBufferSize] = {};
        GetClassNameW(Hwnd, ClassName, ClassNameBufferSize);
        if
        (
            lstrcmpW(ClassName, L"SHELLDLL_DefView") == 0 && !Children->ShellDefView
        ) Children->ShellDefView = Hwnd;

        if
        (
            lstrcmpW(ClassName, L"WorkerW") == 0 && !Children->WorkerW
        ) Children->WorkerW = Hwnd;
        return TRUE;
    }

    FDesktopWindows FindDesktopWindows()
    {
        FDesktopWindows DesktopWnds;
        DesktopWnds.Progman = FindWindowW(L"Progman", nullptr);
        if (!DesktopWnds.Progman)
        {
            Log(L"ERROR: Progman not found!"); return DesktopWnds;
        }

        DWORD_PTR Result = 0;
        SendMessageTimeoutW
        (
            DesktopWnds.Progman,
            WM_SPAWN_WORKERW,
            0,
            0,
            SMTO_NORMAL,
            1000,
            &Result
        );
        Log(L"Sent WM_SPAWN_WORKERW to Progman.");

        HWND DirectShell = FindWindowExW
        (
            DesktopWnds.Progman,
            nullptr,
            L"SHELLDLL_DefView",
            nullptr
        );
        if (DirectShell)
        {
            Log(L"Win11 24H2+ mode.");
            DesktopWnds.ShellDefView = DirectShell;
            DesktopWnds.bShellOnProgman = true;
            FProgmanChildren ProgmanChildren;
            EnumChildWindows
            (
                DesktopWnds.Progman,
                ProgmanChildProc,
                reinterpret_cast<LPARAM>(&ProgmanChildren)
            );
            DesktopWnds.WorkerW = ProgmanChildren.WorkerW;
            return DesktopWnds;
        }

        Log(L"Legacy WorkerW mode.");
        for (int32_t Attempt = 0; Attempt < 20; ++Attempt)
        {
            FLegacySearch LegacySearch;
            EnumWindows(LegacyEnumProc, reinterpret_cast<LPARAM>(&LegacySearch));
            if (LegacySearch.WorkerWithShell && LegacySearch.WorkerWithout)
            {
                DesktopWnds.WorkerW = LegacySearch.WorkerWithout;
                DesktopWnds.ShellDefView = FindWindowExW
                (
                    LegacySearch.WorkerWithShell,
                    nullptr,
                    L"SHELLDLL_DefView",
                    nullptr
                );
                return DesktopWnds;
            }
            Sleep(100);
        }
        Log(L"Legacy mode: timeout.");
        return DesktopWnds;
    }

    BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT InRect, LPARAM LParam)
    {
        auto* Rects = reinterpret_cast<std::vector<RECT>*>(LParam);
        Rects->push_back(*InRect);
        return TRUE;
    }

    std::vector<RECT> EnumerateMonitors()
    {
        std::vector<RECT> Rects;
        EnumDisplayMonitors
        (
            nullptr, 
            nullptr, 
            MonitorEnumProc, 
            reinterpret_cast<LPARAM>(&Rects)
        );
        Log(L"Found " + std::to_wstring(Rects.size()) + L" monitor(s).");
        for (size_t Index = 0; Index < Rects.size(); ++Index)
        {
            auto& MonRect = Rects[Index];
            Log
            (
                L"  Monitor " + std::to_wstring(Index) + L": "
                + std::to_wstring(MonRect.right - MonRect.left) + L"x" + std::to_wstring(MonRect.bottom - MonRect.top)
                + L" at (" + std::to_wstring(MonRect.left) + L"," + std::to_wstring(MonRect.top) + L")"
            );
        }
        return Rects;
    }

    class FMediaPlayerCallback final : public IMFPMediaPlayerCallback
    {
    public:
        explicit FMediaPlayerCallback(int32_t InMonitorIndex) 
            : MonitorIndex(InMonitorIndex) {}

        STDMETHODIMP QueryInterface(REFIID Riid, void** OutPv) override
        {
            if (!OutPv) return E_POINTER;
            if (Riid == __uuidof(IUnknown) || Riid == __uuidof(IMFPMediaPlayerCallback))
            {
                *OutPv = static_cast<IMFPMediaPlayerCallback*>(this);
                AddRef(); return S_OK;
            }
            *OutPv = nullptr; return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&RefCount); }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG Count = InterlockedDecrement(&RefCount);
            if (!Count) delete this;
            return Count;
        }

        void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* Header) override
        {
            if (!Header) return;
            if (MonitorIndex < 0 || MonitorIndex >= static_cast<int32_t>(GMonitors.size())) return;

            auto* Player = GMonitors[MonitorIndex].Player;
            if (!Player) return;

            switch (Header->eEventType)
            {
            case MFP_EVENT_TYPE_MEDIAITEM_SET:
            {
                Log(L"Monitor " + std::to_wstring(MonitorIndex) + L": Playing.");
                auto* Event = reinterpret_cast<MFP_MEDIAITEM_SET_EVENT*>(Header);
                if (Event->pMediaItem)
                {
                    PROPVARIANT DurationVar; PropVariantInit(&DurationVar);
                    if 
                    (
                        SUCCEEDED
                        (
                            Event->pMediaItem->GetDuration
                            (
                                MFP_POSITIONTYPE_100NS, 
                                &DurationVar
                            )
                        )
                    ) GMonitors[MonitorIndex].Duration = DurationVar.hVal.QuadPart;

                    PropVariantClear(&DurationVar);
                }
                Player->Play();
                Player->UpdateVideo();
                break;
            }
            case MFP_EVENT_TYPE_PLAYBACK_ENDED:
                Log(L"Monitor " + std::to_wstring(MonitorIndex) + L": Looping.");
                {
                    PROPVARIANT Position; PropVariantInit(&Position);
                    Position.vt = VT_I8; Position.hVal.QuadPart = 0;
                    Player->SetPosition(MFP_POSITIONTYPE_100NS, &Position);
                    PropVariantClear(&Position);
                    Player->Play();
                }
                break;
            default: break;
            }

            if (FAILED(Header->hrEvent))
            Log
            (
                L"Monitor " + std::to_wstring(MonitorIndex) + L" MFP Error: "
                + std::to_wstring(static_cast<long>(Header->hrEvent))
            );
        }
    private:
        ~FMediaPlayerCallback() = default;
        long RefCount = 1;
        int32_t MonitorIndex = 0;
    };
}

namespace
{
    /** Dynamically checks if DWM is currently cloaking the window. */
    bool IsWindowCloaked(HWND Hwnd)
    {
        typedef HRESULT(WINAPI* DwmGetWindowAttribute_t)(HWND, DWORD, PVOID, DWORD);
        static HMODULE DwmApi = LoadLibraryW(L"dwmapi.dll");
        static DwmGetWindowAttribute_t DwmGetWindowAttributeFn =
          DwmApi ? reinterpret_cast<DwmGetWindowAttribute_t>(GetProcAddress(DwmApi, "DwmGetWindowAttribute")) : nullptr;

        if (DwmGetWindowAttributeFn)
        {
            int32_t Cloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttributeFn(Hwnd, 14, &Cloaked, sizeof(Cloaked))))
            {
                return Cloaked != 0;
            }
        }
        return false;
    }

    /** Returns true if a single window covers at least one full monitor. */
    bool IsWindowCoveringMonitor(HWND Hwnd)
    {
        if (IsZoomed(Hwnd))
            return true;

        LONG_PTR Style = GetWindowLongPtrW(Hwnd, GWL_STYLE);
        bool bNoBorder = !(Style & WS_CAPTION) || !(Style & WS_THICKFRAME);
        if (!bNoBorder) return false;

        RECT WindowRect;
        GetWindowRect(Hwnd, &WindowRect);

        HMONITOR Monitor = MonitorFromWindow(Hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO MonitorInfo = { sizeof(MonitorInfo) };
        GetMonitorInfoW(Monitor, &MonitorInfo);

        return 
        (
            WindowRect.left <= MonitorInfo.rcMonitor.left &&
            WindowRect.top <= MonitorInfo.rcMonitor.top &&
            WindowRect.right >= MonitorInfo.rcMonitor.right &&
            WindowRect.bottom >= MonitorInfo.rcMonitor.bottom
        );
    }

    /** Returns true if a window class belongs to the desktop shell. */
    bool IsShellWindow(HWND Hwnd)
    {
        wchar_t ClassName[ClassNameBufferSize] = {};
        GetClassNameW(Hwnd, ClassName, ClassNameBufferSize);
        return 
        (
            lstrcmpW(ClassName, L"Progman") == 0 ||
            lstrcmpW(ClassName, L"WorkerW") == 0 ||
            lstrcmpW(ClassName, L"Shell_TrayWnd") == 0 ||
            lstrcmpW(ClassName, L"Shell_SecondaryTrayWnd") == 0
        );
    }

    /** EnumWindows callback — sets bOccluded to true if any window covers the desktop. */
    BOOL CALLBACK OcclusionEnumProc(HWND Hwnd, LPARAM LParam)
    {
        bool* bOutOccluded = reinterpret_cast<bool*>(LParam);

        if (!IsWindowVisible(Hwnd)) return TRUE;
        if (IsIconic(Hwnd)) return TRUE;


        LONG_PTR ExStyle = GetWindowLongPtrW(Hwnd, GWL_EXSTYLE);
        if (ExStyle & WS_EX_TOOLWINDOW) return TRUE;

        if (IsWindowCloaked(Hwnd)) return TRUE;

        if (IsShellWindow(Hwnd)) return TRUE;

        for (const auto& Monitor : GMonitors)
        {
            if (Monitor.Window == Hwnd) return TRUE;
        }

        if (IsWindowCoveringMonitor(Hwnd))
        {
            *bOutOccluded = true;
            return FALSE;

        }
        return TRUE;
    }

    /** Checks whether ANY visible top-level window fully covers a monitor. */
    bool IsDesktopOccluded()
    {
        bool bOccluded = false;
        EnumWindows(OcclusionEnumProc, reinterpret_cast<LPARAM>(&bOccluded));
        return bOccluded;
    }
}

LRESULT CALLBACK WallpaperWndProc(HWND Hwnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
    switch (Msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT PaintStruct;
        BeginPaint(Hwnd, &PaintStruct);
        EndPaint(Hwnd, &PaintStruct);
        for (auto& Monitor : GMonitors)
        {
            if (Monitor.Window == Hwnd && Monitor.Player)
            {
                Monitor.Player->UpdateVideo();
            }
        }
            
        return 0;
    }
    case WM_SIZE:
        for (auto& Monitor : GMonitors)
        {
            if (Monitor.Window == Hwnd && Monitor.Player)
            {
                Monitor.Player->UpdateVideo();
            }
        }
            
        return 0;
    }
    return DefWindowProcW(Hwnd, Msg, WParam, LParam);
}

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_QUIT 1001
#define ID_TRAY_PAUSE 1002
#define ID_TRAY_MUTE 1003
#define ID_TRAY_CHANGE_VIDEO 1004
#define ID_TRAY_AUTOSTART 1005

namespace
{
    NOTIFYICONDATAW GTrayIconData = {};

    void AddTrayIcon(HWND Hwnd)
    {
        GTrayIconData.cbSize = sizeof(GTrayIconData);
        GTrayIconData.hWnd = Hwnd;
        GTrayIconData.uID = 1;
        GTrayIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        GTrayIconData.uCallbackMessage = WM_TRAYICON;
        GTrayIconData.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        if (!GTrayIconData.hIcon) GTrayIconData.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy(GTrayIconData.szTip, L"VideoWallpaper");
        Shell_NotifyIconW(NIM_ADD, &GTrayIconData);
    }

    void RemoveTrayIcon()
    {
        Shell_NotifyIconW(NIM_DELETE, &GTrayIconData);
    }

    void ShowTrayMenu(HWND Hwnd)
    {
        HMENU Menu = CreatePopupMenu();
        AppendMenuW(Menu, MF_STRING, ID_TRAY_PAUSE, GbPaused ? L"Resume" : L"Pause");
        AppendMenuW(Menu, MF_STRING, ID_TRAY_MUTE, GbMuted ? L"Unmute" : L"Mute");
        AppendMenuW(Menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(Menu, MF_STRING, ID_TRAY_CHANGE_VIDEO, L"Change Video...");
        AppendMenuW
        (
            Menu, IsAutoStartEnabled() 
            ? (MF_STRING | MF_CHECKED) 
            : MF_STRING, ID_TRAY_AUTOSTART, L"Start with Windows"
        );
        AppendMenuW(Menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(Menu, MF_STRING, ID_TRAY_QUIT, L"Quit VideoWallpaper");

        POINT CursorPos;
        GetCursorPos(&CursorPos);
        SetForegroundWindow(Hwnd);
        TrackPopupMenu
        (
            Menu, 
            TPM_BOTTOMALIGN | TPM_LEFTALIGN, 
            CursorPos.x, 
            CursorPos.y, 
            0, 
            Hwnd, 
            nullptr
        );
        DestroyMenu(Menu);
    }
}

LRESULT CALLBACK MessageWndProc(HWND Hwnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
    switch (Msg)
    {
    case WM_CREATE:
        RegisterHotKey(Hwnd, 1, MOD_CONTROL | MOD_ALT, 'Q');
        RegisterHotKey(Hwnd, 2, MOD_CONTROL | MOD_ALT, 'P');
        SetTimer(Hwnd, TimerIdUpdate, TimerIntervalMs, nullptr);
        AddTrayIcon(Hwnd);
        return 0;
    case WM_HOTKEY:
        if (WParam == 1) DestroyWindow(Hwnd);
        if (WParam == 2) SendMessageW(Hwnd, WM_COMMAND, ID_TRAY_PAUSE, 0);
        return 0;
    case WM_TRAYICON:
        if (LOWORD(LParam) == WM_RBUTTONUP || LOWORD(LParam) == WM_CONTEXTMENU)
            ShowTrayMenu(Hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(WParam))
        {
        case ID_TRAY_QUIT:
            DestroyWindow(Hwnd);
            break;
        case ID_TRAY_PAUSE:
            GbPaused = !GbPaused;
            GbAutoPausedByFullscreen = false;

            for (auto& Monitor : GMonitors)
            {
                if (Monitor.Player) 
                { 
                    GbPaused ? Monitor.Player->Pause() 
                             : Monitor.Player->Play(); 
                }
            }
            break;

        case ID_TRAY_MUTE:
            GbMuted = !GbMuted;
            for (auto& Monitor : GMonitors)
            {
                if (Monitor.Player)
                {
                    Monitor.Player->SetMute(GbMuted ? TRUE : FALSE);
                }
            }
                
            break;
        case ID_TRAY_CHANGE_VIDEO:
            ChangeVideo();
            break;
        case ID_TRAY_AUTOSTART:
            SetAutoStart(!IsAutoStartEnabled());
            break;
        }
        return 0;
    case WM_DISPLAYCHANGE:
        Log(L"Display change detected.");
        {
            auto Rects = EnumerateMonitors();
            for 
            (
                size_t Index = 0; 
                Index < GMonitors.size() && Index < Rects.size(); 
                ++Index
            )
            {
                auto& MonRect = Rects[Index];
                auto& Monitor = GMonitors[Index];
                Monitor.Rect = MonRect;
                HWND Parent = GetParent(Monitor.Window);
                POINT Point = { MonRect.left, MonRect.top };
                if (Parent) MapWindowPoints(nullptr, Parent, &Point, 1);

                SetWindowPos
                (
                    Monitor.Window, 
                    nullptr, 
                    Point.x, 
                    Point.y,
                    MonRect.right - MonRect.left, 
                    MonRect.bottom - MonRect.top,
                    SWP_NOZORDER | SWP_NOACTIVATE
                );
                if (Monitor.Player) Monitor.Player->UpdateVideo();
            }
        }
        return 0;
    case WM_TIMER:
        if (WParam == TimerIdUpdate)
        {
            if (!GbPaused)
            {
                bool bOccluded = IsDesktopOccluded();
                if (bOccluded && !GbAutoPausedByFullscreen)
                {
                    GbAutoPausedByFullscreen = true;
                    for (auto& Monitor : GMonitors)
                    {
                        if (Monitor.Player) Monitor.Player->Pause();
                    }

                    Log(L"Auto-paused: foreground window covers desktop.");
                }
                else if (!bOccluded && GbAutoPausedByFullscreen)
                {
                    GbAutoPausedByFullscreen = false;
                    for (auto& Monitor : GMonitors)
                    {
                        if (Monitor.Player) Monitor.Player->Play();
                    }
                        
                    Log(L"Auto-resumed: desktop visible.");
                }
            }

            for (auto& Monitor : GMonitors)
            {
                if 
                (
                    !Monitor.Player       || 
                    Monitor.Duration <= 0 || 
                    GbPaused              || 
                    GbAutoPausedByFullscreen
                ) continue;

                PROPVARIANT Position; PropVariantInit(&Position);
                if 
                (
                    SUCCEEDED
                    (
                        Monitor.Player->GetPosition(MFP_POSITIONTYPE_100NS, &Position)
                    )
                )
                {
                    LONGLONG CurrentPos = Position.hVal.QuadPart;
                    if (CurrentPos > 0 && (Monitor.Duration - CurrentPos) < PreSeekThreshold100ns)
                    {
                        PROPVARIANT Zero; PropVariantInit(&Zero);
                        Zero.vt = VT_I8; Zero.hVal.QuadPart = 0;
                        Monitor.Player->SetPosition(MFP_POSITIONTYPE_100NS, &Zero);
                        PropVariantClear(&Zero);
                        Log(L"Pre-seek loop triggered.");
                    }
                }
                PropVariantClear(&Position);
            }
        }
        return 0;
    case WM_DESTROY:
        KillTimer(Hwnd, TimerIdUpdate);
        RemoveTrayIcon();
        UnregisterHotKey(Hwnd, 1);
        UnregisterHotKey(Hwnd, 2);
        ShutdownAllMonitors();
        MFShutdown();
        CoUninitialize();
        CloseLog();
        if (GMutex) 
        { 
            ReleaseMutex(GMutex); 
            CloseHandle(GMutex); 
            GMutex = nullptr; 
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(Hwnd, Msg, WParam, LParam);
}

namespace
{
    void ShutdownAllMonitors()
    {
        for (auto& Monitor : GMonitors)
        {
            if (Monitor.Player)
            {
                Monitor.Player->Shutdown();
                Monitor.Player->Release();
                Monitor.Player = nullptr;
            }
            if (Monitor.Window)
            {
                DestroyWindow(Monitor.Window);
                Monitor.Window = nullptr;
            }
        }
        GMonitors.clear();

        if (GDesktop.WorkerW)
        {
            ShowWindow(GDesktop.WorkerW, SW_SHOW);
        }
    }

    bool CreateMonitorWallpapers(const FDesktopWindows& DesktopWnds)
    {
        auto Rects = EnumerateMonitors();
        if (Rects.empty()) return false;

        HWND InsertAfter = DesktopWnds.ShellDefView;


        for (size_t Index = 0; Index < Rects.size(); ++Index)
        {
            auto&& MonRect = Rects[Index];

            int32_t Width = MonRect.right - MonRect.left;
            int32_t Height = MonRect.bottom - MonRect.top;

            FMonitorWallpaper MonWallpaper;
            MonWallpaper.Rect = Rects[Index];


            if (DesktopWnds.bShellOnProgman)
            {
                MonWallpaper.Window = CreateWindowExW
                (
                    0, 
                    GWallpaperClassName, 
                    L"",
                    WS_POPUP | WS_VISIBLE,
                    MonRect.left, 
                    MonRect.top, 
                    Width, 
                    Height,
                    nullptr, 
                    nullptr, 
                    GInstance, 
                    nullptr
                );
                if (!MonWallpaper.Window) 
                {
                    Log
                    (
                        L"Failed to create window for monitor " + std::to_wstring(Index)
                    ); continue; 
                }

                SetParent(MonWallpaper.Window, DesktopWnds.Progman);
                LONG_PTR Style = GetWindowLongPtrW(MonWallpaper.Window, GWL_STYLE);
                Style = (Style & ~WS_POPUP) | WS_CHILD;
                SetWindowLongPtrW(MonWallpaper.Window, GWL_STYLE, Style);

                POINT Point = { MonRect.left, MonRect.top };
                MapWindowPoints(nullptr, DesktopWnds.Progman, &Point, 1);

                Log
                (
                    L"Monitor " + std::to_wstring(Index) + L": screen(" + std::to_wstring(MonRect.left) + L","
                    + std::to_wstring(MonRect.top) + L") -> client(" + std::to_wstring(Point.x) + L"," + std::to_wstring(Point.y) + L")"
                );

                if (InsertAfter)
                {
                    SetWindowPos
                    (
                        MonWallpaper.Window, 
                        InsertAfter, 
                        Point.x, 
                        Point.y, 
                        Width, 
                        Height, 
                        SWP_NOACTIVATE | SWP_SHOWWINDOW
                    );
                }
                else
                {
                    SetWindowPos
                    (
                        MonWallpaper.Window, 
                        HWND_BOTTOM, 
                        Point.x, 
                        Point.y, 
                        Width, 
                        Height, 
                        SWP_NOACTIVATE | SWP_SHOWWINDOW
                    );
                }

                InsertAfter = MonWallpaper.Window;
            }
            else
            {
                HWND Host = DesktopWnds.WorkerW 
                          ? DesktopWnds.WorkerW 
                          : DesktopWnds.Progman;

                POINT Point = { MonRect.left, MonRect.top };
                MapWindowPoints(nullptr, Host, &Point, 1);

                MonWallpaper.Window = CreateWindowExW
                (
                    0, GWallpaperClassName, L"",
                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                    Point.x, Point.y, 
                    Width, Height,
                    Host, 
                    nullptr, 
                    GInstance, 
                    nullptr
                );
                if (!MonWallpaper.Window)
                {
                    Log(L"Failed to create window for monitor " + std::to_wstring(Index)); 
                    continue; 
                }
                SetWindowPos
                (
                    MonWallpaper.Window, 
                    HWND_BOTTOM, 
                    Point.x, Point.y, 
                    Width, Height, 
                    SWP_NOACTIVATE | SWP_SHOWWINDOW
                );
            }

            GMonitors.push_back(MonWallpaper);
            Log
            (
                L"Created window for monitor " + std::to_wstring(Index) + L": "
                + std::to_wstring(Width) + L"x" + std::to_wstring(Height)
            );
        }

        if (DesktopWnds.bShellOnProgman && DesktopWnds.WorkerW)
        {
            ShowWindow(DesktopWnds.WorkerW, SW_HIDE);
            Log(L"Hid static wallpaper WorkerW.");
        }

        return !GMonitors.empty();
    }

    bool CreatePlayers()
    {
        for (size_t Index = 0; Index < GMonitors.size(); ++Index)
        {
            auto& Monitor = GMonitors[Index];
            auto* Callback = new FMediaPlayerCallback(static_cast<int32_t>(Index));
            HRESULT Result = MFPCreateMediaPlayer
            (
                GVideoPath.c_str(), TRUE, 0, Callback, Monitor.Window, &Monitor.Player
            );
            Callback->Release();

            if (FAILED(Result) || !Monitor.Player)
            {
                Log
                (
                    L"MFPCreateMediaPlayer FAILED for monitor " + std::to_wstring(Index)
                    + L" hr=" + std::to_wstring(static_cast<long>(Result))
                );
                return false;
            }

            Monitor.Player->SetMute(GbMuted ? TRUE : FALSE);

            ShowWindow(Monitor.Window, SW_SHOW);
            UpdateWindow(Monitor.Window);
            Log(L"Player created for monitor " + std::to_wstring(Index));
        }
        return true;
    }

    const wchar_t* GAutoRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const wchar_t* GAutoRunValue = L"VideoWallpaper";

    bool IsAutoStartEnabled()
    {
        HKEY Key;
        if 
        (
            RegOpenKeyExW
            (
                HKEY_CURRENT_USER, GAutoRunKey, 0, KEY_READ, &Key
            ) != ERROR_SUCCESS
        ) return false;

        bool bExists = RegQueryValueExW
        (
            Key, GAutoRunValue, nullptr, nullptr, nullptr, nullptr
        ) == ERROR_SUCCESS;

        RegCloseKey(Key);
        return bExists;
    }

    void SetAutoStart(bool bEnable)
    {
        HKEY Key;
        if 
        (
            RegOpenKeyExW
            (
                HKEY_CURRENT_USER, 
                GAutoRunKey, 
                0, 
                KEY_WRITE, 
                &Key
            ) != ERROR_SUCCESS
        )
            return;
        if (bEnable)
        {
            wchar_t ExePath[MAX_PATH];
            GetModuleFileNameW(nullptr, ExePath, MAX_PATH);
            RegSetValueExW
            (
                Key, 
                GAutoRunValue, 
                0, 
                REG_SZ,
                reinterpret_cast<const BYTE*>(ExePath),
                static_cast<DWORD>((wcslen(ExePath) + 1) * sizeof(wchar_t))
            );
        }
        else
        {
            RegDeleteValueW(Key, GAutoRunValue);
        }
        RegCloseKey(Key);
    }

    void ChangeVideo()
    {
        wchar_t FilePath[MAX_PATH] = {};
        OPENFILENAMEW OpenFileName = {};
        OpenFileName.lStructSize = sizeof(OpenFileName);
        OpenFileName.hwndOwner = GMsgWindow;
        OpenFileName.lpstrFilter = 
            L"Video Files\0*.mp4;*.wmv;*.avi;*.mkv;*.mov;*.webm\0All Files\0*.*\0";

        OpenFileName.lpstrFile = FilePath;
        OpenFileName.nMaxFile = MAX_PATH;
        OpenFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        OpenFileName.lpstrTitle = L"Select Wallpaper Video";

        if (!GetOpenFileNameW(&OpenFileName)) return;

        std::wstring ConfigPath = GetExeDir() + L"\\config.txt";
        std::wofstream ConfigFile(ConfigPath.c_str());
        ConfigFile << FilePath;
        ConfigFile.close();

        GVideoPath = FilePath;
        ShutdownAllMonitors();
        Log(L"Reloading video: " + GVideoPath);

        GDesktop = FindDesktopWindows();
        if (!GDesktop.Progman) return;

        if (!GDesktop.bShellOnProgman)
        {
            HWND Host = GDesktop.WorkerW ? GDesktop.WorkerW : GDesktop.Progman;
            ShowWindow(Host, SW_SHOWNA);
        }

        if (!CreateMonitorWallpapers(GDesktop)) return;
        if (!CreatePlayers())
        {
            MessageBoxW
            (
                nullptr, 
                L"Failed to create player for the selected video.",
                L"VideoWallpaper",
                MB_ICONERROR
            );
            return;
        }
        GbPaused = false;
        GbAutoPausedByFullscreen = false;
    }
}

int WINAPI wWinMain(HINSTANCE Instance, HINSTANCE, PWSTR, int)
{
    GInstance = Instance;

    GMutex = CreateMutexW(nullptr, TRUE, L"Global\\VideoWallpaperMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW
        (
            nullptr, 
            L"VideoWallpaper is already running.", 
            L"VideoWallpaper", 
            MB_ICONINFORMATION
        );
        return 0;
    }

    GbDebugEnabled = IsDebugFlagPresent();
    if (GbDebugEnabled) Log(L"Debug logging enabled.");

    GVideoPath = ReadVideoPath();
    if (GVideoPath.empty())
    {
        MessageBoxW
        (
            nullptr, 
            L"config.txt is empty or missing.\n\nPlace a video file path in config.txt next to VideoWallpaper.exe.",
            L"VideoWallpaper", 
            MB_ICONERROR
        );
        return 1;
    }
    if (GetFileAttributesW(GVideoPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::wstring ErrorMsg = L"Video file not found:\n" + GVideoPath;
        MessageBoxW(nullptr, ErrorMsg.c_str(), L"VideoWallpaper", MB_ICONERROR);
        return 1;
    }
    Log(L"Video path: " + GVideoPath);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }

    WNDCLASSW WallpaperWndClass{};
    WallpaperWndClass.lpfnWndProc = WallpaperWndProc;
    WallpaperWndClass.hInstance = Instance;
    WallpaperWndClass.lpszClassName = GWallpaperClassName;
    WallpaperWndClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&WallpaperWndClass);

    const wchar_t* MsgClassName = L"VideoWallpaperMsgClass";
    WNDCLASSW MsgWndClass{};
    MsgWndClass.lpfnWndProc = MessageWndProc;
    MsgWndClass.hInstance = Instance;
    MsgWndClass.lpszClassName = MsgClassName;
    RegisterClassW(&MsgWndClass);

    for (int32_t Attempt = 0; Attempt < MaxDesktopRetries; ++Attempt)
    {
        GDesktop = FindDesktopWindows();
        if (GDesktop.Progman) break;
        Log(L"Desktop not ready, retrying in 1s...");
        Sleep(1000);
    }
    if (!GDesktop.Progman)
    {
        MessageBoxW(nullptr, L"Could not find the desktop window (Progman).", L"VideoWallpaper", MB_ICONERROR);
        MFShutdown(); CoUninitialize();
        return 1;
    }

    if (!GDesktop.bShellOnProgman)
    {
        HWND Host = GDesktop.WorkerW ? GDesktop.WorkerW : GDesktop.Progman;
        ShowWindow(Host, SW_SHOWNA);
    }

    if (!CreateMonitorWallpapers(GDesktop))
    {
        MessageBoxW(nullptr, L"Failed to create wallpaper windows.", L"VideoWallpaper", MB_ICONERROR);
        MFShutdown(); CoUninitialize();
        return 1;
    }

    if (!CreatePlayers())
    {
        std::wstring ErrorMsg = L"Failed to create media player.\n\nFile: " + GVideoPath;
        MessageBoxW(nullptr, ErrorMsg.c_str(), L"VideoWallpaper", MB_ICONERROR);
        ShutdownAllMonitors();
        MFShutdown(); CoUninitialize();
        return 1;
    }

    EmptyWorkingSet(GetCurrentProcess());
    Log(L"Working set trimmed after player init.");

    GMsgWindow = CreateWindowExW
    (
        0, MsgClassName, L"", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, Instance, nullptr
    );
    if (!GMsgWindow)
    {
        Log(L"ERROR: Failed to create message window.");
        ShutdownAllMonitors();
        MFShutdown(); 
        CoUninitialize();
        return 1;
    }

    MSG Msg;
    while (GetMessageW(&Msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
    return static_cast<int>(Msg.wParam);
}