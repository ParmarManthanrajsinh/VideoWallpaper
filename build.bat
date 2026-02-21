@echo off
setlocal

:: Kill any running instance first (prevents "Permission denied")
taskkill /F /IM VideoWallpaper.exe >nul 2>&1

:: Source file
set SOURCE=main.cpp

:: Output executable
set OUTPUT=VideoWallpaper.exe

:: Resource file
set RESOURCE=app.rc
set RESOURCE_OBJ=app_res.o

:: Libraries to link against (MinGW)
set LIBS=-lmfplay -lmfplat -lmfuuid -lmf -lole32 -lshlwapi -lgdi32 -luser32 -lshell32 -lcomdlg32 -ladvapi32 -lpsapi

:: Compiler flags
:: -static to avoid dependency on MinGW DLLs
:: -mwindows to create a GUI application (no console window)
:: -municode for wWinMain entry point
:: -std=c++20 for modern C++
:: -Os optimize for size, -s strip symbols
set FLAGS=-static -mwindows -municode -std=c++20 -Os -s

echo Building %OUTPUT%...

:: Compile resource file
windres %RESOURCE% -o %RESOURCE_OBJ%
if %ERRORLEVEL% NEQ 0 (
    echo Resource compilation failed.
    goto :end
)

:: Compile and link
g++ %SOURCE% %RESOURCE_OBJ% -o %OUTPUT% %FLAGS% %LIBS%

if %ERRORLEVEL% EQU 0 (
    echo Build successful!
    :: Clean up resource object
    del %RESOURCE_OBJ% >nul 2>&1
) else (
    echo Build failed.
)

:end
endlocal
