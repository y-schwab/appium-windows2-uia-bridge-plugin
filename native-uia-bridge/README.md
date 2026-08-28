# native-uia-bridge — internals

This is the native side of `appium-windows2-uia-bridge-plugin`: an injected DLL
(`appium-uia-bridge.dll`) plus a small injector (`appium-uia-bridge-injector.exe`), built for both
`x64` and `Win32` (see `UiaBridge.vcxproj`, `Injector.vcxproj`, `build.bat`).

## File map

| File | Role |
|---|---|
| `Injector.cpp` | Standalone exe. Resolves the target hwnd's pid, injects the DLL (first attach) or re-enters an already-loaded one (subsequent attaches), relays the diag log to stderr. |
| `DllMain.cpp` | DLL entry point. `AttachWorker` does the real work off the loader thread; `ReattachEntryPoint` is the exported re-entry point for subsequent attaches. |
| `WindowSubclass.cpp/.h` | Subclasses a hwnd (`SetWindowLongPtr(GWLP_WNDPROC)`), answers `WM_GETOBJECT` with a `ProviderRoot`, recurses onto every child hwnd. |
| `AccessibleTree.cpp/.h` | The tiered extraction pipeline (`GetNodeInfo`) and MSAA-plus-hwnd child discovery (`GetChildren`). |
| `OleControlTree.cpp/.h` | Tier 2 — OLE native-object-model property reads, SEH-guarded. |
| `GdiTextCapture.cpp/.h` | Tiers 3–4 — GDI/GDI+ paint interception, glyph-index decoding. |
| `ProviderRoot.cpp/.h`, `ProviderElement.cpp/.h` | `IRawElementProviderFragment(Root)`/`IValueProvider`/`IInvokeProvider` implementations UIA actually talks to. |
| `ElementRegistry.cpp/.h` | Keeps located elements alive between find and use. |
| `Diagnostics.cpp/.h` | `DiagLog` — appends to a per-pid temp file; `Injector.cpp` relays it after attach. |
| `ComPtr.h`, `UiaMapping.h` | Small COM smart pointer; MSAA-role → UIA-`ControlType` mapping. |

## The extraction pipeline

See the root `README.md`'s tier table for the user-facing summary. Implementation notes:

- **Tier 0/1** (`AccessibleTree.cpp`): plain MSAA plus a `GetWindowTextW`/`OBJID_WINDOW` fallback.
  Nothing unusual — this is what most controls need.
- **Tier 2** (`OleControlTree.cpp`): `WM_GETOBJECT(OBJID_NATIVEOM)` → `IDispatch`, then
  `Name`/`Caption`/`Text`/`Value`/`ControlTipText`/`Accelerator` property reads via
  `GetIDsOfNames`+`Invoke`. Every `Invoke` call is wrapped in `__try`/`__except` — a third-party
  automation object raising a structured exception degrades this tier to "no answer" instead of
  crashing the host process. This tier was removed once (when it came back empty for the one app
  that originally motivated this codebase) and restored — that was one app's broken accessibility
  plumbing, not evidence the approach itself is dead. Keep it as a live, zero-configuration fallback
  for whatever future target *does* answer `OBJID_NATIVEOM`.
- **Tiers 3/4** (`GdiTextCapture.cpp`): the interesting one. `InstallGdiTextHooksEverywhere` snapshots
  every loaded module and IAT-patches each one's imports for `TextOutA/W`, `ExtTextOutA/W`,
  `DrawTextA/W`, `DrawTextExA/W`, and four GDI+ flat-API functions
  (`GdipCreateFromHDC`/`GdipCreateFromHWND`/`GdipDeleteGraphics`/`GdipDrawString`) — a module that
  doesn't import any of them is an untouched no-op, so this is safe to run broadly rather than
  guessing which module a given app's control library lives in. Two real subtleties this tier
  handles that a naive text-capture hook wouldn't:
  - **`ETO_GLYPHINDEX`**: some controls pre-shape their text (Uniscribe, for RTL scripts) and paint
    raw glyph indices, not characters. There's no built-in glyph→character API, only the reverse
    (`GetGlyphIndicesW`), so the reverse map is built by brute force: run the *font's own declared
    Unicode coverage* (`GetFontUnicodeRanges`, capped at 6000 candidates) through `GetGlyphIndicesW`
    and invert the result, cached per `HFONT`. RTL runs also come out in visual (screen) order, not
    logical (reading) order — reversed only when GDI's own `ETO_RTLREADING` flag says so, not
    unconditionally.
  - **Codepage for zero-extended ANSI**: a captured "wide" string can turn out to be single-byte
    ANSI data that got zero-extended into `wchar_t` one byte at a time instead of properly
    converted (every code point stays under `0x100` — the tell). The codepage used to reinterpret
    it comes from the actual selected font's charset (`GetTextCharset`+`TranslateCharsetInfo`), not
    a hardcoded script.
  - Both a real paint has to actually happen for capture to have anything: these controls typically
    draw once at window-open and never again on their own, so `GetNodeInfo`'s tier-3/4 fallback
    forces one (`InvalidateRect`+`UpdateWindow`, synchronous even cross-thread) before checking the
    capture map.

## Every hwnd needs its own subclass

`InstallSubclass` (`WindowSubclass.cpp`) recurses onto every hwnd `GetChildren()` discovers, not
just the one it was called with. Skipping this means any hwnd-first UIA lookup (Inspect.exe's mouse
hit-test, `NativeWindowHandle`-based resolution, UI Automation Core's own hwnd-boundary re-hosting)
bypasses the tree entirely and falls back to the OS's generic MSAA proxy — confirmed via Inspect's
own `ProviderDescription` reading `Main: Microsoft: MSAA Proxy`, not this DLL, until this was fixed.

A `GetDirectChildWindows`/MSAA-resolved child with zero or negative on-screen area is excluded from
the tree — the textbook case is a ComboBox's own dropdown-list child window, which many
implementations leave `WS_VISIBLE` permanently and just collapse to zero size while closed rather
than truly hiding. This is a live/self-correcting check (real window state queried every call, not
cached), and deliberately geometry-only, not content-based — a zero-size element can never be
clicked or located by point regardless of what control produced it, so this is general rather than
specific to any one app's control library.

## Injection and re-attach

First attach into a process: `Injector.cpp`'s `InjectDll` does the classic
`CreateRemoteThread`+`LoadLibraryW`. `DllMain`'s `DLL_PROCESS_ATTACH` fires exactly once per
process ever (Windows loader semantics) and kicks off `AttachWorker` on its own thread.

Any subsequent `windows: attachUiaBridge` into that same process can't rely on
`DLL_PROCESS_ATTACH` firing again — a second `LoadLibraryW` just bumps the module refcount.
`Injector.cpp` detects this (a `Toolhelp32` module snapshot) and instead calls the exported
`ReattachEntryPoint` directly: computes its RVA against a *locally* loaded copy of the same DLL
file (`LoadLibraryExW`+`DONT_RESOLVE_DLL_REFERENCES` — maps and relocates the PE image normally, so
`GetProcAddress` resolves the export table correctly, without running the DLL's own `DllMain` or
needing its imports resolved), then starts a remote thread at
`target-process-module-base + rva`. RVA is base-independent by construction, which is what makes
this valid despite the two processes' independent ASLR bases for the same DLL image. Everything
downstream — the handshake file, the result file, the diag log relay — is unchanged; both paths
converge on the same protocol once code is actually running again.

**This does not, and structurally cannot, reach a window opened in a genuinely separate process.**
Some legacy multi-process suites launch each dialog as its own `.exe` rather than a new window in
the same process (confirmed on real usage — clicking a button in the target app launched a whole
separate `PARMETER.EXE`). There is no in-process mechanism that can see a process that hasn't
started yet; that would need an out-of-process watcher (something outside any single injected
process — the Node plugin side polling for new sibling processes, or a WMI subscription) triggering
a fresh explicit attach, which does not currently exist. An earlier attempt at automatic
same-process popup detection (`SetWinEventHook`-based) was built, confirmed unable to reach this
actual case (`WINEVENT_INCONTEXT` hooks only ever see events within their own process), and removed
rather than kept as unproven complexity.

## Diagnostics

`Diagnostics.h`'s `DiagLog` appends every trace line to `%TEMP%\appium-uia-bridge-inject-<pid>.diag.log`.
`Injector.cpp` reads and relays the whole file to stderr after every attach attempt (success or
failure), then deletes it. `src/attach.ts` on the TypeScript side splits that into a one-line `info`
summary (always visible) and the full trace at `debug` (visible only when Appium's own logger is
running at debug level) — see that file's `summarizeDiag`.

Logging inside the native code follows the same instinct: only the *winning* extraction tier and
genuinely new information gets logged, not every attempt along the way — a dense UI tree (dozens of
controls, each walking all four tiers) would otherwise drown the log for no signal. `GetContainerAccessible`
and the GDI-capture fallback log once, on their actual outcome, not per intermediate step.

## Known follow-ups

- Multi-line captions only capture their last line through tiers 3/4 (last-write-wins in the
  capture map) — first-line capture is a real gap, not yet fixed.
- `DrawThemeText`/`DrawThemeTextEx` (UxTheme — common in themed custom controls) and DirectWrite
  (`IDWriteTextLayout::Draw`) are two more paint surfaces worth hooking for modern hybrid apps —
  not yet covered by tiers 3/4.
- Cross-process "new window" auto-detection (see above) — no current design, would need to live
  outside any single injected process.
