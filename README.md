# VideoWallpaper

A **super lightweight** live video wallpaper for Windows. Just an `.exe` and a `config.txt` — no installer, no dependencies, no bloat.

## Features

- 🎬 Play any video as your desktop wallpaper (`.mp4`, `.wmv`, `.avi`, etc.)
- 🪶 Single portable `.exe` (~1 MB) with no external dependencies
- 🔁 Seamless video looping
- 🖥️ Multi-monitor support (spans entire virtual desktop)
- 🛡️ Single instance guard (prevents duplicates)
- 📺 Auto-resize on display/resolution change
- 🪟 Supports **Windows 10** (legacy WorkerW) and **Windows 11 24H2+** (Progman child)

## Quick Start

1. Place a video path in `config.txt`:
   ```
   C:\Users\YourName\Videos\wallpaper.mp4
   ```

2. Run `VideoWallpaper.exe`

3. Press **Ctrl + Alt + Q** to quit

## Files

| File | Purpose |
|------|---------|
| `VideoWallpaper.exe` | The application |
| `config.txt` | Video file path (one line, absolute path) |
| `build.bat` | Build script (requires MinGW/g++) |
| `main.cpp` | Full source code |

## Building from Source

**Requirements:** MinGW-w64 (g++ with Media Foundation headers)

```bat
.\build.bat
```

This produces `VideoWallpaper.exe` with static linking (no MinGW DLL dependencies).

## Debug Logging

Logging is **off by default** for zero I/O overhead. To enable:

1. Create an empty file named `debug.flag` next to the `.exe`
2. Run the app — a `debug.log` file will be generated
3. Delete `debug.flag` to disable logging again

## How It Works

The app uses the Windows desktop window hierarchy to render video behind your icons:

```
Desktop
├── Progman (Program Manager)
│   ├── SHELLDLL_DefView (desktop icons)
│   ├── ► VideoWallpaper window ◄ (your video)
│   └── WorkerW (static wallpaper — hidden)
```

Video playback is handled by **Windows Media Foundation** (`MFPlay`), which leverages hardware-accelerated decoding built into Windows — no external codecs or libraries needed.

## License

This project is provided as-is for personal use.
