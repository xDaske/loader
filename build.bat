@echo off
rem Build loader.exe with the MSVC toolchain. No CMake required.
rem Output: dist\loader.exe (static CRT, GUI subsystem, admin manifest).
setlocal
cd /d "%~dp0"

if /i "%~1"=="clean" (
    if exist build rmdir /s /q build
    if exist dist rmdir /s /q dist
    echo   [*] build\ and dist\ removed.
    exit /b 0
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_toolchain

set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_toolchain

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto no_toolchain

where cl >nul 2>&1
if errorlevel 1 goto no_toolchain

if not exist dist mkdir dist
if not exist build mkdir build

rem /MT: static CRT, the exe runs on a clean machine.
rem /utf-8: the sources contain UTF-8 comments.
cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fo:build\ /Fd:build\loader.pdb /Fe:dist\loader.exe loader.cpp cleaner.cpp /link /SUBSYSTEM:WINDOWS "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'" user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib uuid.lib winhttp.lib
if errorlevel 1 goto fail

echo.
echo   [ok] dist\loader.exe
for %%A in ("dist\loader.exe") do echo        %%~zA bytes
exit /b 0

:no_toolchain
echo   [!] MSVC x64 nao encontrado.
echo       Visual Studio Installer -^> "Desktop development with C++"
echo       Componentes: MSVC v143 (ou mais novo) + Windows 11 SDK.
exit /b 1

:fail
echo   [!] Build falhou. Veja as mensagens do cl acima.
exit /b 1
