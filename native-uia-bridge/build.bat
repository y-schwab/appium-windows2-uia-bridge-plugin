@echo off
REM Builds appium-uia-bridge.dll and appium-uia-bridge-injector.exe (pure C++, no /clr), dropping
REM both into native/win-x64/ — same OutDir convention as appium-desktop-driver's own native
REM build scripts (see dotnet-bridge-agent/build.bat there).
REM
REM x64 only in v1: the injector's bitness must match its target process's bitness (a 64-bit
REM process can't be injected into by a 32-bit CreateRemoteThread caller, and vice versa), so
REM 32-bit target support would need a second Win32|Release build of both projects, mirroring
REM appium-desktop-driver's win-x64/win-x86 split — deferred, not required for the v1 scope.
REM
REM Requires: Visual Studio with the "Desktop development with C++" workload. Run from a
REM "Developer Command Prompt for VS" (or after calling vcvars64.bat) so msbuild is on PATH.

setlocal

where msbuild >nul 2>nul
if not errorlevel 1 (
    set "MSBUILD=msbuild"
    goto :build
)

REM Not on PATH (not run from a Developer Command Prompt) — ask vswhere, the tool every VS 2017+
REM installer registers at this fixed location, to locate the newest installed MSBuild instead.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
        set "MSBUILD=%%i"
    )
)

if not defined MSBUILD (
    echo msbuild not found on PATH and vswhere could not locate it.
    echo Run this from a "Developer Command Prompt for VS 2022", or install
    echo the "Desktop development with C++" workload.
    exit /b 1
)

:build
"%MSBUILD%" "%~dp0UiaBridge.vcxproj" /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b 1
echo Built native\win-x64\appium-uia-bridge.dll

"%MSBUILD%" "%~dp0Injector.vcxproj" /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b 1
echo Built native\win-x64\appium-uia-bridge-injector.exe

endlocal
