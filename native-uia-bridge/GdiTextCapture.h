#pragma once
// Path 1 (see NEXT_STEPS.md): GDI text-draw capture. Neither MSAA nor FM20's OLE native-object-
// model answer WM_GETOBJECT anywhere in the target app's tree, despite FM20.DLL genuinely being
// loaded — the host app's own window procedures swallow WM_GETOBJECT before it reaches FM20's
// default handling. Whatever a control actually displays still has to reach the screen via GDI
// though (this is a classic GDI-painted control library, no DirectWrite/Uniscribe path), so
// IAT-patching FM20.DLL's own TextOut*/ExtTextOut*/DrawText* imports and capturing every string
// that passes through — tagged by which hwnd's DC it was painted into (WindowFromDC) — is a
// ground-truth fallback that doesn't care what object/accessibility model backs the control.

#include <string>
#include <windows.h>

namespace UiaBridge {

// Patches TextOutW/A, ExtTextOutW/A, DrawTextW/A, and DrawTextExW/A in `targetModule`'s own
// import address table (IAT) — not gdi32/user32's export table, so this only affects code that
// imports through `targetModule` (i.e. FM20.DLL's own painting, not every other DLL in the
// process). Safe to call more than once; a second call on the same module is a harmless no-op
// (detected via the already-patched pointer). Returns false if `targetModule` is null or no
// matching import was found to patch (e.g. the module doesn't import any of these directly).
bool InstallGdiTextHooks(HMODULE targetModule);

// Calls InstallGdiTextHooks against every module currently loaded in this process (via
// Toolhelp32Snapshot), not just a caller-guessed one. Neither FM20.DLL nor the main EXE's own
// import table had a GdipDrawString entry to patch (see NEXT_STEPS.md / the diag logs that led
// here) — the actual paint calls plausibly live in a third module (e.g. a custom UI helper DLL
// the host app loads), which there's no reliable way to guess up front. Cheap and safe to run
// broadly: patching a module that doesn't import any of these functions is a harmless no-op.
// Returns the number of modules where at least one import was actually patched.
int InstallGdiTextHooksEverywhere();

// Returns the most recently captured painted text for `hwnd`, or empty if nothing has been
// captured for it yet (hook never installed, hwnd never painted through a hooked call, or the
// paint targeted an offscreen/memory DC that WindowFromDC couldn't attribute to any hwnd).
std::wstring GetLastPaintedText(HWND hwnd);

} // namespace UiaBridge
