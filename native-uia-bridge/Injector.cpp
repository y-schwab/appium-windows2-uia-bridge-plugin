// Standalone injector: appium-uia-bridge-injector.exe <hwnd> <dllPath>
//
// Resolves the pid owning `hwnd`, writes the handshake file appium-uia-bridge.dll reads on load
// (see DllMain.cpp), then injects the DLL via CreateRemoteThread+LoadLibraryW — the primary path
// called out by the task spec. Deliberately a separate .exe rather than routed through
// appium-desktop-driver's native host: this plugin is spawned directly by the plugin's Node code
// (see src/attach.ts), with zero coupling to the driver's own native/ build.
//
// SetWindowsHookEx fallback: not implemented in v1. It would be used for the case where
// CreateRemoteThread is blocked (e.g. by some AV/EDR products treating remote-thread creation
// into a foreign process as suspicious) — the fallback shape is to register a global
// WH_GETMESSAGE hook DLL that the target loads automatically when it next pumps a message,
// avoiding CreateRemoteThread/OpenProcess entirely at the cost of needing a matching-bitness
// hook DLL and a running message loop on the target thread. Left as a documented v2 extension;
// CreateRemoteThread covers the acceptance criteria for a normal (non-hardened) legacy app.

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

bool WriteHandshakeFile(DWORD pid, HWND hwnd) {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring path = std::wstring(tempDir) + L"appium-uia-bridge-inject-" + std::to_wstring(pid) + L".hwnd";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::string contents = std::to_string(reinterpret_cast<uintptr_t>(hwnd));
    DWORD written = 0;
    BOOL ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == contents.size();
}

// Injects `dllPath` into `pid` via the classic CreateRemoteThread+LoadLibraryW technique.
// LoadLibraryW's address is identical across processes within one boot session — kernel32 isn't
// independently rebased per process — so resolving it locally and using it as the remote thread's
// start routine works without needing to compute any offset.
bool InjectDll(DWORD pid, const std::wstring& dllPath) {
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!process) {
        fwprintf(stderr, L"OpenProcess failed for pid %lu (error %lu)\n", pid, GetLastError());
        return false;
    }

    SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        fwprintf(stderr, L"VirtualAllocEx failed (error %lu)\n", GetLastError());
        CloseHandle(process);
        return false;
    }

    bool ok = WriteProcessMemory(process, remotePath, dllPath.c_str(), pathBytes, nullptr) != 0;
    if (!ok) {
        fwprintf(stderr, L"WriteProcessMemory failed (error %lu)\n", GetLastError());
    }

    HANDLE thread = nullptr;
    if (ok) {
        auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        thread = CreateRemoteThread(process, nullptr, 0, loadLibraryW, remotePath, 0, nullptr);
        if (!thread) {
            fwprintf(stderr, L"CreateRemoteThread failed (error %lu)\n", GetLastError());
            ok = false;
        }
    }

    if (thread) {
        WaitForSingleObject(thread, 15000);
        DWORD exitCode = 0;
        GetExitCodeThread(thread, &exitCode); // exitCode is the loaded HMODULE, 0 on LoadLibraryW failure
        ok = exitCode != 0;
        if (!ok) {
            fwprintf(stderr, L"LoadLibraryW returned NULL in the target process\n");
        }
        CloseHandle(thread);
    }

    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        fwprintf(stderr, L"Usage: appium-uia-bridge-injector.exe <hwnd> <dllPath>\n");
        return 1;
    }

    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(argv[1], nullptr, 10)));
    if (!IsWindow(hwnd)) {
        fwprintf(stderr, L"hwnd %p is not a valid window\n", hwnd);
        return 1;
    }

    DWORD pid = 0;
    if (!GetWindowThreadProcessId(hwnd, &pid) || pid == 0) {
        fwprintf(stderr, L"Could not resolve the process owning hwnd %p\n", hwnd);
        return 1;
    }

    if (!WriteHandshakeFile(pid, hwnd)) {
        fwprintf(stderr, L"Failed to write the injection handshake file for pid %lu\n", pid);
        return 1;
    }

    if (!InjectDll(pid, argv[2])) {
        return 1;
    }

    // Plugin side (src/attach.ts) only needs a success/failure signal — the exit code — since the
    // DLL self-installs the subclass once loaded; nothing further is read from stdout, but the
    // pid is still useful for troubleshooting a failed attach.
    wprintf(L"%lu\n", pid);
    return 0;
}
