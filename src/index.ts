import { BasePlugin } from 'appium/plugin';
import type { ExecuteMethodMap, ExternalDriver, NextPluginCallback } from '@appium/types';
import { attachUiaBridge as runAttach } from './attach.js';

/**
 * Injects `appium-uia-bridge.dll` into the process that owns `elementId` (typically the
 * `F3 Server 60000000` / Forms 2.0 container hwnd). Reads `NativeWindowHandle` via the driver's
 * standard `getProperty` — the same generic, unfiltered UIA property passthrough
 * appium-desktop-driver already uses internally — so this plugin needs no driver-side changes to
 * resolve the target hwnd.
 *
 * Nothing else is needed after this resolves: once the DLL answers WM_GETOBJECT for that hwnd,
 * the container's children become part of the real UI Automation tree, stitched in via
 * UiaHostProviderFromHwnd. The driver's ordinary findElement/click/getText/`windows: invoke`
 * commands reach them with no plugin-specific find/invoke surface required.
 */
async function attachUiaBridge(
    this: UiaBridgePlugin,
    _next: NextPluginCallback,
    driver: ExternalDriver,
    elementId: string,
): Promise<void> {
    // Server-side driver method — matches appium-desktop-driver's own lib/commands/element.ts
    // `getProperty(propertyName, elementId)`, not the client-facing WebDriver command name
    // (`getElementProperty`), which doesn't exist on the driver instance itself.
    const hwndProperty = await (driver as ExternalDriver & {
        getProperty: (name: string, elementId: string) => Promise<string>;
    }).getProperty('NativeWindowHandle', elementId);
    const hwnd = Number(hwndProperty);
    if (!Number.isInteger(hwnd) || hwnd <= 0) {
        throw new Error(`Element ${elementId} did not report a valid NativeWindowHandle (got: ${hwndProperty})`);
    }
    await runAttach(hwnd);
}

export class UiaBridgePlugin extends BasePlugin {
    static override executeMethodMap: ExecuteMethodMap<UiaBridgePlugin> = {
        'windows: attachUiaBridge': { command: 'attachUiaBridge', params: { required: ['elementId'] } },
    };

    attachUiaBridge = attachUiaBridge;

    /**
     * Appium's plugin dispatcher (`AppiumDriver.pluginsToHandleCmd`) only gives a plugin a turn
     * for a given HTTP command if the plugin instance defines a method with that command's exact
     * name — for the classic `execute`/`executeScript` endpoint that name is `execute`.
     * `BasePlugin` only auto-implements `executeMethod` (a *different* name, which checks
     * `executeMethodMap`), never `execute` itself — so a plugin that only declares
     * `executeMethodMap` is invisible to the dispatcher for this route and never gets asked.
     * Overriding `execute` here and delegating into the inherited `executeMethod` (which does the
     * real `executeMethodMap` lookup and calls `next()` for anything it doesn't recognize) is what
     * actually makes `windows: attachUiaBridge` reachable via `driver.executeScript(...)`.
     */
    async execute(next: NextPluginCallback, driver: ExternalDriver, script: string, args: unknown[]): Promise<unknown> {
        return await this.executeMethod(next, driver, script, args as [Record<string, unknown>]);
    }
}

export default UiaBridgePlugin;
