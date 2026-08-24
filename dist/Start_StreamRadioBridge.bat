@echo off
setlocal EnableExtensions EnableDelayedExpansion

title Stream Radio Bridge 1.0.5
set "BRIDGE_DIR=%~dp0Native\StreamRadioBridge"
set "BRIDGE_DLL=%BRIDGE_DIR%\StreamRadioBridge.dll"
set "BRIDGE_INJECTOR=%BRIDGE_DIR%\StreamRadioBridgeInject.exe"
set "YTDLP=%BRIDGE_DIR%\yt-dlp.exe"
set "FFMPEG=%BRIDGE_DIR%\ffmpeg.exe"

if not exist "%BRIDGE_DLL%" (
    echo [ERROR] StreamRadioBridge.dll was not found.
    pause
    exit /b 2
)
if not exist "%BRIDGE_INJECTOR%" (
    echo [ERROR] StreamRadioBridgeInject.exe was not found.
    pause
    exit /b 3
)
if not exist "%YTDLP%" (
    echo [ERROR] yt-dlp.exe was not found.
    pause
    exit /b 4
)
if not exist "%FFMPEG%" (
    echo [ERROR] ffmpeg.exe was not found.
    pause
    exit /b 5
)

echo Starting Scrap Mechanic through Steam...
echo The bridge will wait up to 120 seconds and inject automatically.
echo Run this BAT on every player PC that should hear Stream Radio.
echo.

tasklist /FI "IMAGENAME eq ScrapMechanic.exe" 2>nul | find /I "ScrapMechanic.exe" 1>nul
if errorlevel 1 (
    set "STEAM_EXE="
    for /f "tokens=2,*" %%A in ('reg query "HKCU\Software\Valve\Steam" /v SteamExe 2^>nul') do set "STEAM_EXE=%%B"
    if defined STEAM_EXE if exist "!STEAM_EXE!" (
        echo Steam detected at: !STEAM_EXE!
        start "" "!STEAM_EXE!" -applaunch 387990
    ) else (
        echo Steam registry path was not found; using the Steam URL protocol.
        start "" "steam://rungameid/387990"
    )
) else (
    echo Scrap Mechanic is already running; it will not be launched twice.
)
"%BRIDGE_INJECTOR%" "%BRIDGE_DLL%"
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" pause
exit /b %RESULT%
