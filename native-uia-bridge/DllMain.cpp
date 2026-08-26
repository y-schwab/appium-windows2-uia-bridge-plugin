// Entry point for appium-uia-bridge.dll. Loaded via CreateRemoteThread+LoadLibraryW by
// Injector.cpp (see build.bat / Injector.cpp for the injection side). Self-installs the UIA
// subclass on load — nothing further needs to run inside the target process afterward, and
// nothing outside the process needs to talk to this DLL at runtime; see the plan doc's
// "Plugin surface (runtime, minimal)" section for why no IPC channel is needed here.

#include <windows.h>
#include <combaseapi.h>
#include <cstdlib>
#include <string>

#include "Diagnostics.h"
#include "WindowSubclass.h"

namespace {

std::wstring HandshakeFilePath() {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"appium-uia-bridge-inject-" + std::to_wstring(GetCurrentProcessId()) + L".hwnd";
}

// Injector.cpp polls for this after LoadLibraryW returns — see its own copy of this path (must
// match exactly, same pid-keyed naming as HandshakeFilePath above). Lets the injector block on
// the *real* InstallSubclass outcome instead of exiting the moment the DLL finishes loading,
// which used to race ahead of AttachWorker even starting.
std::wstring ResultFilePath() {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"appium-uia-bridge-inject-" + std::to_wstring(GetCurrentProcessId()) + L".result";
}

// First byte is '1' (success) or '0' (failure); anything after that on a failure is a
// human-readable reason (UTF-8), read back and surfaced by Injector.cpp -> attach.ts's rejection.
void WriteResultFile(bool success, const std::wstring& errorMessage) {
    HANDLE file = CreateFileW(ResultFilePath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return; // Injector's own poll timeout is the fallback signal if even this fails.
    }
    std::string contents(1, success ? '1' : '0');
    if (!success && !errorMessage.empty()) {
        int len = WideCharToMultiByte(CP_UTF8, 0, errorMessage.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string utf8(static_cast<size_t>(len) - 1, '\0'); // len includes the null terminator
            WideCharToMultiByte(CP_UTF8, 0, errorMessage.c_str(), -1, utf8.data(), len, nullptr, nullptr);
            contents += utf8;
        }
    }
    DWORD written = 0;
    WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr);
    CloseHandle(file);
}

// Injector.cpp writes this file (containing the target hwnd as decimal text) immediately before
// triggering LoadLibraryW, keyed by this process's own pid — which the injector already knows,
// having resolved it via GetWindowThreadProcessId(hwnd) to open the process in the first place.
bool ReadHandshakeHwnd(HWND* outHwnd) {
    std::wstring path = HandshakeFilePath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    char buffer[64] = {};
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
    CloseHandle(file);
    DeleteFileW(path.c_str());

    if (!ok || bytesRead == 0) {
        return false;
    }

    buffer[bytesRead] = '\0';
    *outHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::strtoull(buffer, nullptr, 10)));
    return *outHwnd != nullptr;
}

DWORD WINAPI AttachWorker(LPVOID) {
    UiaBridge::DiagLog(L"AttachWorker started, pid=%lu", GetCurrentProcessId());

    // Decisive, cheap check for what's actually behind the target's control classname before
    // assuming anything about it (see LogLoadedModules' own doc comment) — process-wide info, so
    // logged unconditionally here rather than gated behind hwnd/attach success.
    UiaBridge::LogLoadedModules();

    // This is a bare CreateThread thread — no apartment is initialized on it by anything else in
    // the process. GetContainerAccessible's ObjectFromLresult call unmarshals a COM interface
    // pointer out of the WM_GETOBJECT reply, and per its own documentation requires
    // CoInitialize/OleInitialize to have been called first on the calling thread; without this,
    // it's running against an uninitialized apartment, which is a plausible source of intermittent
    // E_FAIL results depending on what else in the host process happened to touch COM first.
    HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(comInit);
    UiaBridge::DiagLog(L"CoInitializeEx(APARTMENTTHREADED) -> hr=0x%08lX", static_cast<unsigned long>(comInit));

    // Injection races the handshake file write against LoadLibraryW's DllMain call — both
    // happen on the injector's side before the remote thread starts, so the file is guaranteed
    // to exist by the time this runs. A short retry loop covers any residual scheduling jitter
    // (e.g. antivirus intercepting the file write) without hanging indefinitely.
    HWND hwnd = nullptr;
    int handshakeAttempts = 0;
    for (; handshakeAttempts < 20 && !ReadHandshakeHwnd(&hwnd); ++handshakeAttempts) {
        Sleep(50);
    }
    if (!hwnd) {
        UiaBridge::DiagLog(L"Handshake file never appeared or contained no valid hwnd after %d attempts (%dms)", handshakeAttempts, handshakeAttempts * 50);
        WriteResultFile(false, L"handshake file never appeared or contained no valid hwnd");
        if (comInitialized) { CoUninitialize(); }
        return 0;
    }
    UiaBridge::DiagLog(L"Handshake hwnd resolved after %d attempt(s): hwnd=0x%p", handshakeAttempts + 1, hwnd);

    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    wchar_t windowText[256] = {};
    GetWindowTextW(hwnd, windowText, ARRAYSIZE(windowText));
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    DWORD ownerPid = 0;
    DWORD ownerTid = GetWindowThreadProcessId(hwnd, &ownerPid);
    UiaBridge::DiagLog(
        L"Target hwnd details: class=\"%s\" windowText=\"%s\" rect={l:%ld,t:%ld,r:%ld,b:%ld} "
        L"visible=%d enabled=%d ownerTid=%lu ownerPid=%lu (this process pid=%lu)",
        className, windowText, rect.left, rect.top, rect.right, rect.bottom,
        IsWindowVisible(hwnd), IsWindowEnabled(hwnd), ownerTid, ownerPid, GetCurrentProcessId());

    std::wstring error;
    bool ok = UiaBridge::InstallSubclass(hwnd, &error);
    UiaBridge::DiagLog(L"InstallSubclass -> %s%s%s", ok ? L"success" : L"FAILED", ok ? L"" : L": ", ok ? L"" : error.c_str());
    WriteResultFile(ok, error);
    if (comInitialized) { CoUninitialize(); }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            // Do the real work on a worker thread — DllMain must not block or make complex COM
            // calls while the loader lock is held.
            CloseHandle(CreateThread(nullptr, 0, AttachWorker, nullptr, 0, nullptr));
            break;
        case DLL_PROCESS_DETACH:
            UiaBridge::RemoveAllSubclasses();
            break;
    }
    return TRUE;
}
