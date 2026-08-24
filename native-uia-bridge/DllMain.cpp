// Entry point for appium-uia-bridge.dll. Loaded via CreateRemoteThread+LoadLibraryW by
// Injector.cpp (see build.bat / Injector.cpp for the injection side). Self-installs the UIA
// subclass on load — nothing further needs to run inside the target process afterward, and
// nothing outside the process needs to talk to this DLL at runtime; see the plan doc's
// "Plugin surface (runtime, minimal)" section for why no IPC channel is needed here.

#include <windows.h>
#include <cstdlib>
#include <string>

#include "WindowSubclass.h"

namespace {

std::wstring HandshakeFilePath() {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"appium-uia-bridge-inject-" + std::to_wstring(GetCurrentProcessId()) + L".hwnd";
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
    // Injection races the handshake file write against LoadLibraryW's DllMain call — both
    // happen on the injector's side before the remote thread starts, so the file is guaranteed
    // to exist by the time this runs. A short retry loop covers any residual scheduling jitter
    // (e.g. antivirus intercepting the file write) without hanging indefinitely.
    HWND hwnd = nullptr;
    for (int attempt = 0; attempt < 20 && !ReadHandshakeHwnd(&hwnd); ++attempt) {
        Sleep(50);
    }
    if (!hwnd) {
        return 0;
    }
    UiaBridge::InstallSubclass(hwnd);
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
