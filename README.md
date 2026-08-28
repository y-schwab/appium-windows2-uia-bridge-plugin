# appium-windows2-uia-bridge-plugin

In-process UI Automation bridge for legacy Win32 controls whose accessibility is broken or
absent — as an installable Appium plugin for
[appium-desktop-driver](https://github.com/verisoft-ai/appium-desktop-driver).

## The problem

Some legacy Windows apps (classic Win32, VB6/Forms 2.0, custom-drawn control libraries) never wire
up real accessibility support. Out-of-process UIA/MSAA queries against them — from Inspect.exe,
this driver, or any other automation tool — come back with `Name=""`, `[null]` values, and opaque
`Pane`-typed controls, because there's simply nothing behind the standard APIs to answer.

## The fix

This plugin injects a small native DLL (`appium-uia-bridge.dll`) directly into the target process
and subclasses every relevant window's procedure to answer `WM_GETOBJECT` with a real UI Automation
provider — built **in-process**, so it can read whatever information is actually available on that
control, through whichever channel it happens to expose. There's no single API this relies on:
it tries the good sources first and only falls back to more invasive ones when a control has
genuinely nothing else to offer.

### Extraction tiers

Every control's name/value is resolved by trying these in order, stopping at the first one that
produces something — see `GetNodeInfo` in `native-uia-bridge/AccessibleTree.cpp`, the single place
all four are consulted per element.

| Tier | Source | Fires when |
|---|---|---|
| 0 | MSAA (`accName`/`accValue`/`accDescription`/`accKeyboardShortcut`) | the control implements real accessibility (most controls, eventually) |
| 1 | `GetWindowTextW` / `OBJID_WINDOW` probe | window text is set even without real MSAA |
| 2 | OLE embedding model / native-object-model (`WM_GETOBJECT(OBJID_NATIVEOM)`) | a genuine VBA/Forms 2.0 control whose container actually answers this |
| 3 | GDI paint capture, character mode | the control paints its text via `TextOut`/`DrawText`/`GdipDrawString` |
| 4 | GDI paint capture, glyph-index mode | the control pre-shapes its text before painting (`ETO_GLYPHINDEX` — common for RTL scripts) and only glyph indices ever reach GDI |

Tiers 3–4 work by IAT-patching every loaded module's relevant GDI/GDI+ imports (a control library
that draws its own controls can live in any DLL the process loads, not just the obvious ones —
finding it is part of the job) and capturing whatever text actually gets painted to the screen,
tagged by which window's DC it went into. Tier 4's glyph-to-character mapping is derived from the
actual font's own declared Unicode coverage (`GetFontUnicodeRanges`), not a hardcoded script — see
`native-uia-bridge/GdiTextCapture.cpp` for the full mechanism and `native-uia-bridge/OleControlTree.cpp`
for tier 2.

Every tier is best-effort: a failure at any point (a missing API, an unhelpful COM object, no
candidate glyph match) falls through to the next tier or a safe default, never a crash — tier 2's
COM calls in particular are structured-exception-guarded, since they run arbitrary third-party code.

### Every hwnd gets its own provider

It isn't enough to answer `WM_GETOBJECT` on the top-level container: any hwnd-first lookup (a mouse
hit-test in Inspect.exe, a `NativeWindowHandle`-based resolution, UI Automation Core's own
hwnd-boundary re-hosting) queries a child window's `WM_GETOBJECT` directly, bypassing whatever tree
the root's own provider would otherwise expose. `InstallSubclass` (`native-uia-bridge/WindowSubclass.cpp`)
recurses onto every discovered child hwnd too, each becoming its own fragment root — standard UIA
multi-hwnd embedding, stitched back into one coherent tree automatically via
`UiaHostProviderFromHwnd`.

## Install

```bash
appium plugin install --source=npm appium-windows2-uia-bridge-plugin
```

Requires Appium 3, `appium-desktop-driver` installed and running a `DesktopDriver` session, and the
native artifacts built (`npm run build:native` in this package, or a prebuilt release — see
[Development](#development)). Both 32-bit and 64-bit targets are supported; the plugin resolves the
target process's actual bitness at attach time and picks the matching native binary automatically.

## Enable

```bash
appium --use-plugins=windows2-uia-bridge
```

## Usage

```js
// Switch to the window you want bridged — attach reads the driver's *current* window, not a
// caller-supplied element, so there's no ambiguity about which hwnd is meant.
await driver.setWindow(windowHandle);

// Inject the bridge into the process that owns the current window.
await driver.executeScript('windows: attachUiaBridge', []);

// From here on, use the driver's normal API — no plugin-specific find/invoke surface.
const button = await driver.$('//*[@Name="עזרה"]');
await button.click();
```

| Command | Params | Description |
|---|---|---|
| `windows: attachUiaBridge` | none | Injects the bridge into the process owning the driver's current window. Resolves once that window (and everything under it) is part of the real UIA tree. |

### Re-attaching, and windows the app opens later

Calling `windows: attachUiaBridge` again — on the same window, a different window in the same
process, or a window in a process already bridged — works correctly and re-runs attach logic even
though the DLL is already loaded (Windows only fires `DLL_PROCESS_ATTACH` once per process ever;
this plugin routes around that — see `native-uia-bridge/Injector.cpp`'s `CallReattachEntryPoint`).

**A window the target app opens in a genuinely separate process** (some legacy suites spawn each
dialog as its own `.exe` rather than a new window in the same process) needs its own explicit
`windows: attachUiaBridge` call once you've switched to it — there is currently no automatic
cross-process detection. Confirmed on real usage that this is the common case for "new window"
scenarios with these apps; treat a fresh explicit attach as the normal flow, not a workaround.

## Diagnostics

Every attach logs a one-line summary at `info` level (attached/failed, target window, element
count). The full per-element trace (every extraction tier attempted, every module's GDI imports
patched) logs at `debug` — visible automatically when Appium's own logger is running at debug
level, no separate flag needed.

## Scope / known limitations

- Cross-process "new windows" need an explicit re-attach — see above.
- A multi-line caption currently only captures its last line through the GDI-capture tiers (3–4) —
  first-line capture is a tracked follow-up, not a limitation of the approach.
- GDI capture only intercepts classic GDI and GDI+ paint calls; a control drawn purely through
  DirectWrite (`IDWriteTextLayout::Draw`) or themed text APIs (`DrawThemeText`/`DrawThemeTextEx`)
  isn't covered yet.

## Development

```bash
npm install
npm run build:native   # requires Visual Studio with the "Desktop development with C++" workload
npm run build
npm run test
npm run lint
```

`npm run test:e2e` requires a running Appium server with this plugin and `appium-desktop-driver`
installed, and `E2E_UIA_TARGET_APP_ID` pointing at a legacy target app.

See [`native-uia-bridge/README.md`](native-uia-bridge/README.md) for the native side's internals —
the extraction pipeline, the GDI capture mechanism in detail, and the injection/re-attach protocol.

## License

Apache-2.0
