#pragma once
// Auto-attaches to new top-level windows (dialogs, message boxes, popups) the target app opens
// after the initial `windows: attachUiaBridge` call, so a tester doesn't have to manually re-issue
// that command every time the app under test opens a new window. Same InstallSubclass code path
// as the manual attach — this only changes what triggers it.

#include <windows.h>

namespace UiaBridge {

// Registers an in-context WinEvent hook (runs directly inside this already-injected DLL, no
// cross-process marshaling) watching for EVENT_SYSTEM_FOREGROUND / EVENT_SYSTEM_DIALOGSTART in
// this process, and calls InstallSubclass on each new top-level window it sees. No content
// filtering by class name — a dropdown/tooltip/menu is worth automating just as much as a real
// dialog, and InstallSubclass is already defensive enough (every step logs-and-returns-false
// rather than crashing) that subclassing something transient just costs a wasted call, not
// correctness. The one filter that *is* applied — `GetAncestor(hwnd, GA_ROOT) == hwnd` — isn't
// content filtering, it's picking the right granularity of event: only top-level windows are
// handled here, because InstallSubclass's own recursion onto every child hwnd (WindowSubclass.cpp)
// already covers everything underneath once the top-level one is subclassed. Safe to call more
// than once; a second call is a no-op.
void InstallPopupWatcher();

// Unhooks. Called from DLL_PROCESS_DETACH.
void RemovePopupWatcher();

} // namespace UiaBridge
