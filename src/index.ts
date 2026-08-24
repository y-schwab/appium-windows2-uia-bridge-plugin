import { BasePlugin } from 'appium/plugin';
import type { ExecuteMethodMap, ExternalDriver, NextPluginCallback } from '@appium/types';
import { attachUiaBridge as runAttach } from './attach.js';

/**
 * Injects `appium-uia-bridge.dll` into the process that owns `elementId` (typically the
 * `F3 Server 60000000` / Forms 2.0 container hwnd). Reads `NativeWindowHandle` via the driver's
 * standard `getElementProperty` — the same generic, unfiltered UIA property passthrough
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
    const hwndProperty = await (driver as ExternalDriver & {
        getElementProperty: (name: string, elementId: string) => Promise<string>;
    }).getElementProperty('NativeWindowHandle', elementId);
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
}

export default UiaBridgePlugin;
