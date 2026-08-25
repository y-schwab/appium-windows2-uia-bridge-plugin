#include "Diagnostics.h"
#include <cstdarg>
#include <cstdio>
#include <string>

namespace UiaBridge {

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
