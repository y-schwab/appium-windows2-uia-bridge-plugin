import { describe, it, beforeAll, afterAll, expect } from 'vitest';
import type { Browser } from 'webdriverio';
import { createTargetSession, quitSession } from './helpers/session.js';

/**
 * Exercises the whole point of this plugin: after `windows: attachUiaBridge`, the driver's
 * *ordinary* findElement/click/getText commands should reach children of the F3 Server / Forms
 * 2.0 container that were previously invisible ([null] name/role/value) to any out-of-process
 * UIA client. There's no plugin-specific find/invoke surface to test — if this passes, the
 * container's children are genuinely part of the real UI Automation tree.
 */
describe('windows: attachUiaBridge', () => {
    let driver: Browser;

    beforeAll(async () => {
        driver = await createTargetSession();
    });

    afterAll(async () => {
        await quitSession(driver);
    });

    it('before attach: the container reports no usable children', async () => {
        const container = await driver.$('//*[@ClassName="F3 Server 60000000"]');
        await expect(container.isExisting()).resolves.toBe(true);

        const children = await container.$$('*');
        expect(children.length).toBe(0);
    });

    it('after attach: normal findElement/click reach a child control by name', async () => {
        const container = await driver.$('//*[@ClassName="F3 Server 60000000"]');
        await driver.executeScript('windows: attachUiaBridge', [{ elementId: container.elementId }]);

        // No plugin-specific locator — this is the driver's standard xpath/name find, now able to
        // see into the container because the real UIA tree carries answers instead of [null].
        const button = await driver.$('~OK');
        await expect(button.isExisting()).resolves.toBe(true);
        await expect(button.isEnabled()).resolves.toBe(true);

        await button.click();
    });

    it('reads a text control value through the standard getText command', async () => {
        const label = await driver.$('~StatusLabel');
        await expect(label.isExisting()).resolves.toBe(true);
        expect(typeof await label.getText()).toBe('string');
    });
});
