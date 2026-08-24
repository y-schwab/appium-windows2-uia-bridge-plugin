#pragma once
// Installs the subclass on the target F3 Server / Forms 2.0 container hwnd and answers
// WM_GETOBJECT with our custom UIA provider (see ProviderRoot.h). Uses
// SetWindowLongPtr(GWLP_WNDPROC) directly (one of the two approaches the task calls out,
// alongside SetWindowSubclass) so we keep an explicit handle on the *original* window procedure
// — needed to fetch the container's real in-process IAccessible via
// AccessibleTree::GetContainerAccessible without recursing into our own subclass.

#include <windows.h>

namespace UiaBridge {

// Subclasses `hwnd`, builds the ProviderRoot for it, and registers both so WM_GETOBJECT can be
// answered. Returns false if the container's own IAccessible could not be obtained (nothing to
// bridge) or the hwnd is already subclassed by this DLL.
bool InstallSubclass(HWND hwnd);

// Restores the original window procedure and releases the associated ProviderRoot. Called from
// the subclass's own WM_NCDESTROY handler and from DllMain's DLL_PROCESS_DETACH as a safety net
// for the case where the process is torn down without the window ever being destroyed first.
void RemoveSubclass(HWND hwnd);

void RemoveAllSubclasses();

} // namespace UiaBridge
