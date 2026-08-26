import { BasePlugin } from 'appium/plugin';
import type { ExecuteMethodMap, ExternalDriver, NextPluginCallback } from '@appium/types';
import { attachUiaBridge as runAttach } from './attach.js';

/**
 * Injects `appium-uia-bridge.dll` into the process owning the driver's *current* window — the
 * one the caller already switched to (e.g. via `setWindow`/`changeRootElement`) before calling
 * this, typically the `F3 Server 60000000` / Forms 2.0 container hwnd. Takes no parameters:
 * rather than trust a caller-supplied `elementId` (which may point at some arbitrary located
 * element, not necessarily the session's actual top-level container window), this reads
 * `NativeWindowHandle` off the driver's own root element — `saveRootElementToTable`, the same
 * command appium-desktop-driver uses internally everywhere it needs "whatever window the session
 * is currently on" (see e.g. its lib/commands/app.ts) — so this plugin needs no driver-side
 * changes to resolve the target hwnd, and no ambiguity about which element's hwnd is meant.
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
): Promise<void> {
    // Both are server-side driver methods, not client-facing WebDriver command names — matches
    // appium-desktop-driver's own internal usage (lib/driver.ts's sendCommand, lib/commands/
    // element.ts's getProperty(propertyName, elementId)) — neither exists on the generic
    // ExternalDriver type, hence the inline cast.
    const typedDriver = driver as ExternalDriver & {
        sendCommand: (method: string, params?: Record<string, unknown>) => Promise<unknown>;
        getProperty: (name: string, elementId: string) => Promise<string>;
    };
    const rootElementId = await typedDriver.sendCommand('saveRootElementToTable', {}) as string;
    const hwndProperty = await typedDriver.getProperty('NativeWindowHandle', rootElementId);
    const hwnd = Number(hwndProperty);
    if (!Number.isInteger(hwnd) || hwnd <= 0) {
        throw new Error(`The driver's current window did not report a valid NativeWindowHandle (got: ${hwndProperty}) — switch to the right window (e.g. via setWindow) before calling windows: attachUiaBridge.`);
    }
    await runAttach(hwnd);
}

export class UiaBridgePlugin extends BasePlugin {
    static override executeMethodMap: ExecuteMethodMap<UiaBridgePlugin> = {
        'windows: attachUiaBridge': { command: 'attachUiaBridge' },
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
