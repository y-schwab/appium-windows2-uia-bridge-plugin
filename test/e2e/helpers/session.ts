import { remote } from 'webdriverio';
import type { Browser } from 'webdriverio';

export const APPIUM_SERVER = {
    hostname: '127.0.0.1',
    port: 4723,
    path: '/',
};

type Caps = WebdriverIO.Capabilities;

/**
 * `appId` should point at a legacy app hosting an `F3 Server 60000000` (Microsoft Forms 2.0)
 * container — set the E2E_UIA_TARGET_APP_ID env var to whatever local target app is available;
 * there's no bundled fixture in this repo (see the plan's verification section).
 */
export async function createTargetSession(extraCaps?: Record<string, unknown>): Promise<Browser> {
    const appId = process.env.E2E_UIA_TARGET_APP_ID;
    if (!appId) {
        throw new Error('E2E_UIA_TARGET_APP_ID must be set to a legacy Forms 2.0 target app path/id to run this suite.');
    }
    const driver = await remote({
        ...APPIUM_SERVER,
        capabilities: {
            platformName: 'Windows',
            'appium:automationName': 'DesktopDriver',
            'appium:app': appId,
            'appium:plugins': ['windows2-uia-bridge'],
            ...extraCaps,
        } as Caps,
    });
    await driver.setTimeout({ implicit: 1500 });
    return driver;
}

export async function quitSession(driver: Browser | null): Promise<void> {
    try {
        await driver?.deleteSession();
    } catch {
        // noop — session may already be terminated
    }
}
