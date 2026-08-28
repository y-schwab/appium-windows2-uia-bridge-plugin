#include "PopupWatcher.h"
#include "Diagnostics.h"
#include "WindowSubclass.h"

#include <string>

#pragma comment(lib, "user32.lib") // SetWinEventHook/UnhookWinEvent

namespace UiaBridge {

namespace {

HWINEVENTHOOK g_foregroundHook = nullptr;
HWINEVENTHOOK g_dialogStartHook = nullptr;
HWINEVENTHOOK g_objectShowHook = nullptr;

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    // idObject/idChild filter out the per-element noise WinEvents also carry (e.g. a caret move or
    // a specific child item change reported under the same event ID) — OBJID_WINDOW/CHILDID_SELF
    // is "the window itself," which is the granularity InstallSubclass operates at.
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) { return; }

    DWORD ownerPid = 0;
    GetWindowThreadProcessId(hwnd, &ownerPid);
    if (ownerPid != GetCurrentProcessId()) { return; } // SetWinEventHook is already scoped to this pid, but cheap to double-check

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) { return; } // only top-level windows — see PopupWatcher.h

    std::wstring error;
    if (InstallSubclass(hwnd, &error)) {
        wchar_t className[256] = {};
        GetClassNameW(hwnd, className, ARRAYSIZE(className));
        DiagLog(L"PopupWatcher: auto-attached to new top-level window hwnd=0x%p class=\"%s\" (event=0x%lX)", hwnd, className, event);
    }
    // Failure is unremarkable here — the same hwnd can fire both EVENT_SYSTEM_FOREGROUND and
    // EVENT_SYSTEM_DIALOGSTART, and InstallSubclass's own "already subclassed" guard turns the
    // second one into an expected no-op; InstallSubclass already logs real failure reasons.
}

} // namespace

void InstallPopupWatcher() {
    if (g_foregroundHook || g_dialogStartHook || g_objectShowHook) { return; } // already installed

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&WinEventProc), &self);

    DWORD pid = GetCurrentProcessId();
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, self, WinEventProc, pid, 0, WINEVENT_INCONTEXT);
    g_dialogStartHook = SetWinEventHook(EVENT_SYSTEM_DIALOGSTART, EVENT_SYSTEM_DIALOGSTART, self, WinEventProc, pid, 0, WINEVENT_INCONTEXT);
    // EVENT_SYSTEM_DIALOGSTART only fires for windows created through the real Win32
    // DialogBox/CreateDialog APIs, and EVENT_SYSTEM_FOREGROUND only for a window that actually
    // steals OS foreground focus — this app's custom UI framework (DlgLibrary.dll et al.) has
    // shown a consistent pattern of routing around standard Windows APIs, so neither is reliable
    // here. EVENT_OBJECT_SHOW fires whenever any window becomes visible via ShowWindow, regardless
    // of which API created it or whether it takes focus — the most universal of the three, kept
    // alongside the other two rather than replacing them since a genuine standards-compliant app
    // still benefits from the more specific signals firing first/being cheaper to filter.
    g_objectShowHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, self, WinEventProc, pid, 0, WINEVENT_INCONTEXT);

    DiagLog(L"InstallPopupWatcher: foreground hook %s, dialog-start hook %s, object-show hook %s",
        g_foregroundHook ? L"installed" : L"FAILED", g_dialogStartHook ? L"installed" : L"FAILED",
        g_objectShowHook ? L"installed" : L"FAILED");
}

void RemovePopupWatcher() {
    if (g_foregroundHook) { UnhookWinEvent(g_foregroundHook); g_foregroundHook = nullptr; }
    if (g_dialogStartHook) { UnhookWinEvent(g_dialogStartHook); g_dialogStartHook = nullptr; }
    if (g_objectShowHook) { UnhookWinEvent(g_objectShowHook); g_objectShowHook = nullptr; }
}

} // namespace UiaBridge
