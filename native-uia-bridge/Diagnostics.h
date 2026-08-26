#pragma once
// Verbose, always-on attach-time tracing — separate from ResultFilePath's terse pass/fail reason
// (DllMain.cpp/Injector.cpp). Every DiagLog call appends one timestamped line to this process's
// per-attach log file; Injector.cpp reads the whole thing back after attach finishes (success OR
// failure) and relays it to stderr, so attach.ts's caller sees the full trace regardless of
// outcome — see attach.ts for why that's surfaced even on success.

#include <string>
#include <windows.h>

namespace UiaBridge {

// %TEMP%\appium-uia-bridge-inject-<pid>.diag.log — must use the same pid-keyed naming convention
// as HandshakeFilePath/ResultFilePath elsewhere in this codebase (DllMain.cpp, Injector.cpp).
std::wstring DiagLogPath();

// printf-style; each call opens, appends, and closes the log file, so it survives this thread
// dying mid-attach and needs no shared handle/lock (attach is effectively single-threaded, but
// this makes no assumption of that).
void DiagLog(const wchar_t* fmt, ...);

} // namespace UiaBridge
