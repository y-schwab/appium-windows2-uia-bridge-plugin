import { spawn } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));

/**
 * Native artifacts are gitignored and built by native-uia-bridge/build.bat (see package.json's
 * build:native script) — same "optional, built at publish/install time" shape as
 * appium-desktop-driver's native/ directory.
 */
const NATIVE_DIR = join(__dirname, '..', '..', 'native', 'win-x64');
const INJECTOR_EXE = join(NATIVE_DIR, 'appium-uia-bridge-injector.exe');
const BRIDGE_DLL = join(NATIVE_DIR, 'appium-uia-bridge.dll');

/**
 * Runs `appium-uia-bridge-injector.exe <hwnd> <dllPath>`. The injector resolves the owning pid
 * from the hwnd itself (`GetWindowThreadProcessId`), injects `appium-uia-bridge.dll`, and the DLL
 * self-installs the UIA subclass on load — there's nothing left to hand back to Node afterward.
 * Once this resolves, the container's children are part of the real UIA tree: the driver's
 * ordinary `findElement`/`click`/`getText`/etc. reach them with no further plugin involvement.
 */
export async function attachUiaBridge(hwnd: number): Promise<void> {
    await new Promise<void>((resolve, reject) => {
        const child = spawn(INJECTOR_EXE, [String(hwnd), BRIDGE_DLL], { windowsHide: true });
        let stderr = '';
        child.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
        child.on('error', reject);
        child.on('exit', (code) => {
            if (code !== 0) {
                reject(new Error(`appium-uia-bridge-injector.exe exited with code ${code}: ${stderr.trim()}`));
                return;
            }
            resolve();
        });
    });
}
