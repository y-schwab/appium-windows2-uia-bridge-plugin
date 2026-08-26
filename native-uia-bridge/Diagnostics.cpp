#include "Diagnostics.h"
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include <tlhelp32.h>

namespace UiaBridge {

namespace {

// Known Forms 2.0 / VB6-runtime-adjacent module names worth calling out explicitly rather than
// making the reader eyeball the full dump for them — presence/absence of FM20.DLL in particular is
// the single fact that confirms or rules out the whole "genuine VBA UserForm" theory for
// "F3 Server 60000000". The rest round out the same family (VB6's own runtime, common OCX/common-
// control hosts a VB6 or Forms 2.0 app would plausibly also load) or are generic enough
// (MFC/ATL/GDI+) to be worth knowing about if this turns out to be some other framework entirely.
constexpr const wchar_t* kNotableModules[] = {
    L"FM20.DLL",       // Microsoft Forms 2.0 — the one that actually matters here
    L"MSVBVM60.DLL",   // VB6 runtime
    L"MSVBVM50.DLL",   // VB5 runtime
    L"VB40032.DLL",    // VB4 runtime
    L"MSCOMCTL.OCX",   // common controls (VB-hosted)
    L"COMCTL32.DLL",
    L"RICHED32.DLL",
    L"RICHED20.DLL",
    L"OLEAUT32.DLL",
    L"MFC42.DLL",
    L"MFC140.DLL",
    L"ATL.DLL",
    L"ATL100.DLL",
    L"GDIPLUS.DLL",
};

} // namespace

void LogLoadedModules() {
    DiagLog(L"LogLoadedModules: checking known Forms2.0/VB6-runtime-adjacent module presence via GetModuleHandleW:");
    for (const wchar_t* name : kNotableModules) {
        HMODULE h = GetModuleHandleW(name);
        DiagLog(L"  %-14s %s", name, h ? L"LOADED" : L"not loaded");
    }

    // Full raw dump — GetModuleHandleW's curated list above can't anticipate a homegrown/
    // third-party control DLL's actual name, so this is the "grep everything" fallback: every
    // module this process has loaded, name + full path, so a human (or a later grep over the log)
    // can spot whatever's actually implementing this control if it isn't one of the above.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        DiagLog(L"LogLoadedModules: CreateToolhelp32Snapshot failed (error %lu)", GetLastError());
        return;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    if (Module32FirstW(snapshot, &entry)) {
        DiagLog(L"LogLoadedModules: full module list (name @ base — path):");
        do {
            DiagLog(L"  [%d] %s @ 0x%p — %s", count, entry.szModule, entry.modBaseAddr, entry.szExePath);
            ++count;
        } while (Module32NextW(snapshot, &entry));
    } else {
        DiagLog(L"LogLoadedModules: Module32FirstW failed (error %lu)", GetLastError());
    }
    CloseHandle(snapshot);
    DiagLog(L"LogLoadedModules: %d modules total", count);
}

std::wstring DiagLogPath() {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"appium-uia-bridge-inject-" + std::to_wstring(GetCurrentProcessId()) + L".diag.log";
}

void DiagLog(const wchar_t* fmt, ...) {
    wchar_t message[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(message, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1152];
    _snwprintf_s(line, _TRUNCATE, L"[%02u:%02u:%02u.%03u] %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);

    // FILE_APPEND_DATA + OPEN_ALWAYS: each call is a fresh open/write/close, no handle kept open
    // across the (many) call sites this is used from — simplest way to be safe against this
    // running from a background thread that could be killed mid-attach.
    HANDLE file = CreateFileW(DiagLogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len > 0) {
        std::string utf8(static_cast<size_t>(utf8Len) - 1, '\0'); // utf8Len includes the null terminator
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), utf8Len, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}

} // namespace UiaBridge
