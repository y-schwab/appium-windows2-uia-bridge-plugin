@echo off
REM Builds appium-uia-bridge.dll and appium-uia-bridge-injector.exe (pure C++, no /clr) for both
REM x64 and Win32 (x86), dropping them into native/win-x64/ and native/win-x86/ respectively —
REM same OutDir convention as appium-desktop-driver's own native build scripts (see
REM dotnet-bridge-agent/build.bat there). Both bitnesses are required: the injector's bitness
REM must match its target process's bitness (a 64-bit CreateRemoteThread caller can't validly
REM inject into a 32-bit/WOW64 target, and vice versa — confirmed in practice against a real
REM 32-bit legacy target), and this driver is meant to attach to whatever bitness the legacy app
REM actually is. The plugin picks the matching pair at runtime (see src/attach.ts).
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

"%MSBUILD%" "%~dp0UiaBridge.vcxproj" /p:Configuration=Release /p:Platform=Win32
if errorlevel 1 exit /b 1
echo Built native\win-x86\appium-uia-bridge.dll

"%MSBUILD%" "%~dp0Injector.vcxproj" /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b 1
echo Built native\win-x64\appium-uia-bridge-injector.exe

"%MSBUILD%" "%~dp0Injector.vcxproj" /p:Configuration=Release /p:Platform=Win32
if errorlevel 1 exit /b 1
echo Built native\win-x86\appium-uia-bridge-injector.exe

endlocal
