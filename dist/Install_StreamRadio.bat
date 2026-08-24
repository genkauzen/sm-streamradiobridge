@echo off
setlocal EnableExtensions EnableDelayedExpansion

title Stream Radio Core - Installer
color 0A

echo ================================================
echo   Stream Radio Core - Scrap Mechanic installer
echo ================================================
echo.

if not "%STREAMRADIO_DRYRUN%"=="1" (
    tasklist /FI "IMAGENAME eq ScrapMechanic.exe" 2>nul | find /I "ScrapMechanic.exe" 1>nul
    if not errorlevel 1 (
        echo [ERROR] Scrap Mechanic is running.
        echo Close the game and run this installer again.
        pause
        exit /b 2
    )
)

set "SCRIPT_DIR=%~dp0"
set "SOURCE=%SCRIPT_DIR%StreamRadio_1.0.5"
set "USER_ROOT=%APPDATA%\Axolot Games\Scrap Mechanic\User"

if not exist "%SOURCE%\description.json" (
    echo [ERROR] Source mod folder was not found:
    echo         %SOURCE%
    echo Keep this BAT beside the StreamRadio_1.0.5 folder.
    pause
    exit /b 3
)

if not exist "%USER_ROOT%" (
    echo [ERROR] Scrap Mechanic user folder was not found:
    echo         %USER_ROOT%
    echo Start Scrap Mechanic once, close it, and run this installer again.
    pause
    exit /b 4
)

set "USER_DIR="
for /f "delims=" %%U in ('dir /b /ad /o-d "%USER_ROOT%\User_*" 2^>nul') do (
    if not defined USER_DIR set "USER_DIR=%USER_ROOT%\%%U"
)

if not defined USER_DIR (
    echo [ERROR] No Scrap Mechanic profile User_... was found in:
    echo         %USER_ROOT%
    pause
    exit /b 5
)

set "DEST=%USER_DIR%\Mods\StreamRadio_1.0.5"
if not exist "%USER_DIR%\Mods" mkdir "%USER_DIR%\Mods" 2^>nul
if not exist "%USER_DIR%\Mods" (
    echo [ERROR] Could not create the local Mods folder:
    echo         %USER_DIR%\Mods
    pause
    exit /b 6
)

echo Profile detected:
echo   %USER_DIR%
echo.
echo Installing to:
echo   %DEST%
echo.

if "%STREAMRADIO_DRYRUN%"=="1" (
    echo [DRY RUN] No files were copied.
    exit /b 0
)

if exist "%DEST%\Cache" (
    echo Clearing generated mod cache...
    rmdir /s /q "%DEST%\Cache"
)

if exist "%DEST%\StreamRadio_1.0.5\description.json" (
    echo Removing stale nested copy of this mod...
    rmdir /s /q "%DEST%\StreamRadio_1.0.5"
)

robocopy "%SOURCE%" "%DEST%" /E /COPY:DAT /R:2 /W:1 /NFL /NDL /NJH /NJS >nul
set "COPY_RESULT=%ERRORLEVEL%"

if %COPY_RESULT% GEQ 8 (
    echo [ERROR] Copy failed. Robocopy code: %COPY_RESULT%
    pause
    exit /b %COPY_RESULT%
)

echo.
echo [OK] Stream Radio Core was installed.
echo.
echo Next steps:
echo   1. Start the game with Start_StreamRadioBridge.bat.
echo   2. Create or open a world.
echo   3. Enable Stream Radio Core in the world's Mods list.
echo   4. For Survival, use Fant Mod 3 and enable this mod there too.
echo.
echo The native 1.0.5 bridge is included beside this installer.
echo.
pause
exit /b 0
