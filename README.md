# appium-windows2-uia-bridge-plugin

In-process UI Automation provider for legacy windowless controls — `F3 Server 60000000` / Microsoft Forms 2.0 containers — as an installable Appium plugin for [appium-desktop-driver](https://github.com/verisoft-ai/appium-desktop-driver).

## The problem

Legacy C++ apps that host Forms 2.0 controls render them inside a container that never creates separate child `HWND`s. Out-of-process UIA/MSAA queries against that container — from Inspect.exe, this driver, or any other automation tool — return `[null]` names, roles, and values, because the container never marshals its internal controls across the process boundary.

## The fix

This plugin injects a small native DLL (`appium-uia-bridge.dll`) directly into the target process, subclasses the container's window procedure, and answers `WM_GETOBJECT` with a real UI Automation provider built by walking the container's own `IAccessible` tree **in-process** — no marshaling loss, because it's the same code path the app's own UI would use.

Once attached, the container's children become part of the real UI Automation tree via `UiaHostProviderFromHwnd`. There is no plugin-specific find/invoke surface: the driver's ordinary `findElement`, `click`, `getText`, `windows: invoke`, etc. reach the previously-`[null]` children automatically, because the OS's own UI Automation core is now serving real answers instead of empty ones.

## Install

```bash
appium plugin install --source=npm appium-windows2-uia-bridge-plugin
```

Requires Appium 3, `appium-desktop-driver` installed and running a `DesktopDriver` session, and the native artifacts built (`npm run build:native` in this package, or a prebuilt release — see [Development](#development)).

## Enable

```bash
appium --use-plugins=windows2-uia-bridge
```

## Usage

```js
// Find the legacy container (by class name, exactly like any other element).
const container = await driver.$('//*[@ClassName="F3 Server 60000000"]');

// Inject the bridge into the process that owns it.
await driver.executeScript('windows: attachUiaBridge', [{ elementId: container.elementId }]);

// From here on, use the driver's normal API — no plugin-specific commands.
const button = await driver.$('~OK');
await button.click();
```

| Command | Params | Description |
|---|---|---|
| `windows: attachUiaBridge` | `elementId` (required) — the container element | Injects the bridge into the owning process. Resolves once the container is part of the real UIA tree. |

## Scope (v1)

- One F3 Server / Forms 2.0 container per target process. A second `windows: attachUiaBridge` call against a different container in an *already-injected* process is a no-op — `LoadLibrary` just bumps the module refcount without re-running `DllMain` a second time.
- 64-bit target processes only. A 64-bit injector can't inject into a 32-bit process (and vice versa) — 32-bit support would need a parallel `Win32|Release` build, deferred.
- Locator matching against these controls' `IAccessible`s is limited to name and role — classic MSAA has no first-class AutomationId or ClassName concept the way UIA does, so `accessibility id` falls back to name matching and `class name` falls back to the mapped role.

## Development

```bash
npm install
npm run build:native   # requires Visual Studio with the "Desktop development with C++" workload
npm run build
npm run test
npm run lint
```

`npm run test:e2e` requires a running Appium server with this plugin and `appium-desktop-driver` installed, and `E2E_UIA_TARGET_APP_ID` pointing at a legacy Forms 2.0 target app.

## License

Apache-2.0
