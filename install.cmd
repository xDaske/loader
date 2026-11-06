@echo off
setlocal EnableExtensions EnableDelayedExpansion
title loader
color 0B

rem ---------- Config ----------
rem Replace with your repository. The release asset must be named loader.exe.
set "REPO=xDaske/loader"
set "ASSET=loader.exe"
set "DLURL=https://github.com/%REPO%/releases/latest/download/%ASSET%"
set "DIR=%TEMP%"

echo(
echo   loader
echo   ---------------------------------------------
echo(

rem ---------- Arguments ----------
rem   (no arg) = portable: download to %TEMP%, run, nothing installed
rem   p        = same as default (explicit)
rem   i        = install: %TEMP%\loader.exe + boot.cmd + onlogon task
rem   elev     = elevated instance (internal, install mode only)
set "MODE=portable"
set "ELEVATED="
set "TOK="
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="p" set "MODE=portable"
if /i "%~1"=="i" set "MODE=install"
if /i "%~1"=="install" set "MODE=install"
if /i "%~1"=="elev" set "ELEVATED=1"
if /i "%~1"=="elev" set "TOK=%~2"
shift
goto parse
:parsed

rem ---------- Local build (optional) ----------
rem Uncomment and point to your output dir to test without publishing.
rem set "LOCALDIR=C:\path\to\build"
set "LOCALEXE="
if defined LOCALDIR if exist "%LOCALDIR%" (
    for /f "delims=" %%F in ('dir /b /a-d /o-d "%LOCALDIR%\*.exe"') do (
        if not defined LOCALEXE (
            for %%S in ("%LOCALDIR%\%%F") do (
                if %%~zS GEQ 500000 set "LOCALEXE=%LOCALDIR%\%%F"
            )
        )
    )
)
if defined LOCALEXE echo   [*] Local build: %LOCALEXE%

rem Portable deliberately skips the whole install path: no admin prompt here
rem (the exe manifest triggers UAC on its own), no files left behind.
if /i "%MODE%"=="portable" goto portable

rem ---------- Admin (install mode) ----------
if defined ELEVATED goto admin_ok

fltmc >nul 2>&1
if not errorlevel 1 goto admin_ok

if "%TOK%"=="" set "TOK=%RANDOM%%RANDOM%"
set "OKF=%TEMP%\yb_%TOK%.ok"
del /f /q "%OKF%" >nul 2>&1

echo   [*] Requesting administrator rights
echo       Click "Yes" on the Windows prompt.
echo(

set "ELEVARGS=elev %TOK%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '%ELEVARGS%' -Verb RunAs" >nul 2>&1

set "N=0"
:wait_elev
if exist "%OKF%" goto elev_ok
set /a N+=1
if %N% GEQ 30 goto elev_denied
ping -n 2 127.0.0.1 >nul
goto wait_elev

:elev_ok
del /f /q "%OKF%" >nul 2>&1
exit /b 0

:elev_denied
echo   [!] No administrator rights.
echo       Open the prompt as administrator and paste the command again.
echo   [!] Closing in 20 seconds.
timeout /t 20 >nul
exit /b 1

:admin_ok
if not "%TOK%"=="" > "%TEMP%\yb_%TOK%.ok" echo ok

rem ---------- Install ----------
set "EXE=%DIR%\%ASSET%"
set "BAK=%DIR%\loader.dat"
set "BOOT=%DIR%\boot.cmd"

echo   [*] Installing to %DIR%
if not exist "%DIR%" md "%DIR%" >nul 2>&1
if not exist "%DIR%" goto fail_copy

taskkill /f /im "%ASSET%" >nul 2>&1
ping -n 2 127.0.0.1 >nul
if exist "%EXE%" del /f /q "%EXE%" >nul 2>&1

if defined LOCALEXE (
    echo   [*] Copying local build ...
    copy /y "%LOCALEXE%" "%EXE%"
) else (
    echo   [*] Downloading current version ...
    curl -L --fail --silent --show-error --retry 2 -o "%EXE%" "%DLURL%" 2>nul
)

set "SIZE="
if exist "%EXE%" for %%A in ("%EXE%") do set "SIZE=%%~zA"
if not defined SIZE goto fail_dl
if %SIZE% LSS 500000 goto fail_dl

rem Backup: Destruct deletes the exe, boot.cmd restores it.
copy /y "%EXE%" "%BAK%" >nul 2>&1

rem ---------- boot.cmd ----------
> "%BOOT%" echo @echo off
>> "%BOOT%" echo cd /d "%%~dp0"
>> "%BOOT%" echo tasklist /fi "imagename eq %ASSET%" ^| find /i "%ASSET%" ^>nul ^&^& exit /b
>> "%BOOT%" echo if not exist "%ASSET%" if exist "loader.dat" copy /y "loader.dat" "%ASSET%" ^>nul 2^>^&1
>> "%BOOT%" echo if not exist "%ASSET%" curl -L --fail -s -o "%ASSET%" "%DLURL%"
>> "%BOOT%" echo if exist "%ASSET%" start "" "%ASSET%"
>> "%BOOT%" echo exit /b

rem ---------- Autostart ----------
rem Same task names the Destruct removes in clean_launcher_traces.
set "AUTOSTART="
for %%N in (NVOverlayHost NvOverlayHost2 NvOverlayTask) do (
    if not defined AUTOSTART (
        schtasks /create /tn "%%N" /tr "\"%BOOT%\"" /sc onlogon /rl highest /f >nul 2>&1
        if not errorlevel 1 (
            schtasks /query /tn "%%N" >nul 2>&1
            if not errorlevel 1 set "AUTOSTART=%%N"
        )
    )
)
if not defined AUTOSTART (
    reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "NVOverlayHost" /t REG_SZ /d "\"%BOOT%\"" /f >nul 2>&1
    if errorlevel 1 goto fail_task
    set "AUTOSTART=Run-Key"
)

rem ---------- Old leftovers ----------
if exist "%TEMP%\wz" rd /s /q "%TEMP%\wz" >nul 2>&1

echo   [*] %SIZE% bytes - OK
echo   [*] Autostart: %AUTOSTART%
echo   ---------------------------------------------
echo(
echo   [!] Reboot the PC now.
echo       After boot the loader starts by itself.
echo(
echo   [*] Reboot in 15 seconds.
echo       Any key opens the loader now.
timeout /t 15

if errorlevel 1 goto start_now

shutdown /r /t 10 /c "loader" >nul 2>&1
echo(
echo   [*] Rebooting ...
exit /b 0

:start_now
echo(
start "" "%EXE%"
echo   [*] loader started.
exit /b 0

rem ---------- Portable ----------
:portable
set "PORT=%TEMP%\loader_%RANDOM%%RANDOM%.exe"
echo   [*] Downloading to %TEMP%
echo   [*] Nothing installed: no folder, no autostart, no task.
if defined LOCALEXE (
    copy /y "%LOCALEXE%" "%PORT%"
) else (
    curl -sfL --retry 2 -o "%PORT%" "%DLURL%"
)

set "SIZE="
if exist "%PORT%" for %%A in ("%PORT%") do set "SIZE=%%~zA"
if not defined SIZE goto fail_dl
if %SIZE% LSS 500000 goto fail_dl

start "" "%PORT%"
echo   [*] %SIZE% bytes - started.
echo   [*] Destruct deletes the file again on exit.

rem One-liner flow: the installer landed in %TEMP%, so remove it too. The
rem running instance keeps no handle on it, which makes this safe.
if /i "%~dp0"=="%TEMP%\" (
    (goto) 2>nul & del /f /q "%~f0" >nul 2>&1
)
exit /b 0

:fail_dl
rem A failed download leaves a 0-byte file behind; remove it so it is not
rem mistaken for a Destruct leftover on the next inspection.
if defined PORT if exist "%PORT%" del /f /q "%PORT%" >nul 2>&1
if defined EXE if exist "%EXE%" del /f /q "%EXE%" >nul 2>&1
echo   [!] Download failed or the file was blocked.
echo       Check the antivirus and the connection, then run again.
echo       %DLURL%
echo   [!] Closing in 15 seconds.
timeout /t 15 >nul
exit /b 1

:fail_copy
echo   [!] Could not create the folder: %DIR%
echo   [!] Closing in 15 seconds.
timeout /t 15 >nul
exit /b 1

:fail_task
echo   [!] Could not create the autostart entry.
echo       Open the prompt as administrator and run the command again.
echo   [!] Closing in 15 seconds.
timeout /t 15 >nul
exit /b 1
