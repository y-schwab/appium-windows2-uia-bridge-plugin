import { spawn } from 'node:child_process';
import { join } from 'node:path';
import { logger } from '@appium/support';

// Plain console.log does not reliably reach Appium's own log stream/formatting — matches
// appium-desktop-driver's own logging pattern (see its lib/util.js), which is what actually
// surfaces in the server's log output regardless of how stdout is being captured/filtered.
const log = logger.getLogger('uia-bridge');

// __dirname is a CommonJS global (this package builds to CJS, matching @appium/tsconfig's
// default and how Appium plugins are conventionally loaded) — no import.meta/fileURLToPath
// workaround needed.

/**
 * Native artifacts are gitignored and built by native-uia-bridge/build.bat (see package.json's
 * build:native script) — same "optional, built at publish/install time" shape as
 * appium-desktop-driver's native/ directory. Both win-x64/ and win-x86/ pairs are built; which
 * one gets used is decided per-attach, at runtime, from the target's actual bitness.
 */
const NATIVE_ROOT = join(__dirname, '..', '..', 'native');

type Bitness = 'x64' | 'x86';

function nativeDir(bitness: Bitness): string {
    return join(NATIVE_ROOT, bitness === 'x64' ? 'win-x64' : 'win-x86');
}

/**
 * A 64-bit injector cannot validly `CreateRemoteThread` into a 32-bit (WOW64) target process —
 * the `LoadLibraryW` address resolved from a 64-bit process is meaningless in a 32-bit target's
 * address space (confirmed in practice: the remote thread runs but reports the library handle as
 * NULL, not a hard CreateRemoteThread failure — a bitness mismatch does not fail fast here). So
 * the injector/DLL pair must match the target process's actual bitness, checked via
 * `IsWow64Process` on the process that owns `hwnd`. Shelled out to PowerShell rather than adding
 * a native Node addon/FFI dependency just for one bitness check.
 */
async function resolveTargetBitness(hwnd: number): Promise<Bitness> {
    const script = `
$sig = @'
using System;
using System.Runtime.InteropServices;
public class UiaBridgeBitnessCheck {
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool IsWow64Process(IntPtr hProcess, out bool Wow64Process);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr hObject);
}
'@
Add-Type -TypeDefinition $sig
$hwnd = [IntPtr]${hwnd}
# NOT $pid — that's PowerShell's reserved automatic variable holding *this* powershell.exe's own
# process id. Assigning to it fails with a non-terminating "read-only or constant" error and the
# script silently carries on using the original (wrong) value, which is exactly how this bug
# first shipped: it silently checked this host process's own bitness instead of the target's.
$targetPid = 0
[void][UiaBridgeBitnessCheck]::GetWindowThreadProcessId($hwnd, [ref]$targetPid)
if ($targetPid -eq 0) { throw "Could not resolve the process owning hwnd ${hwnd}" }
$hProcess = [UiaBridgeBitnessCheck]::OpenProcess(0x0400, $false, $targetPid) # PROCESS_QUERY_INFORMATION
if ($hProcess -eq [IntPtr]::Zero) { throw "OpenProcess failed for pid $targetPid" }
try {
    $wow64 = $false
    [void][UiaBridgeBitnessCheck]::IsWow64Process($hProcess, [ref]$wow64)
    if ($wow64) { Write-Output "x86" } else { Write-Output "x64" }
} finally {
    [void][UiaBridgeBitnessCheck]::CloseHandle($hProcess)
}
`.trim();

    return await new Promise<Bitness>((resolve, reject) => {
        const child = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script], { windowsHide: true });
        let stdout = '';
        let stderr = '';
        child.stdout.on('data', (chunk) => { stdout += chunk.toString(); });
        child.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
        child.on('error', reject);
        child.on('exit', (code) => {
            const result = stdout.trim();
            if (code !== 0 || (result !== 'x64' && result !== 'x86')) {
                reject(new Error(`Could not determine bitness for hwnd ${hwnd}: ${stderr.trim() || result}`));
                return;
            }
            resolve(result);
        });
    });
}

/**
 * Runs `appium-uia-bridge-injector.exe <hwnd> <dllPath>` — the injector/DLL pair matching the
 * target process's own bitness. The injector resolves the owning pid from the hwnd itself
 * (`GetWindowThreadProcessId`), injects `appium-uia-bridge.dll`, and the DLL self-installs the
 * UIA subclass on load — there's nothing left to hand back to Node afterward. Once this resolves,
 * the container's children are part of the real UIA tree: the driver's ordinary
 * `findElement`/`click`/`getText`/etc. reach them with no further plugin involvement.
 */
export async function attachUiaBridge(hwnd: number): Promise<void> {
    const bitness = await resolveTargetBitness(hwnd);
    const dir = nativeDir(bitness);
    const injectorExe = join(dir, 'appium-uia-bridge-injector.exe');
    const bridgeDll = join(dir, 'appium-uia-bridge.dll');

    await new Promise<void>((resolve, reject) => {
        const child = spawn(injectorExe, [String(hwnd), bridgeDll], { windowsHide: true });
        let stderr = '';
        child.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
        child.on('error', reject);
        child.on('exit', (code) => {
            if (code !== 0) {
                // Same detailed stderr as before (target hwnd's class/rect/enabled state, which
                // WM_GETOBJECT path answered, every child found — see Injector.cpp's
                // RelayDiagLog) — already carried in this rejection's message, so not also
                // console.log'd here to avoid printing it twice.
                reject(new Error(`appium-uia-bridge-injector.exe (${bitness}) exited with code ${code}: ${stderr.trim()}`));
                return;
            }
            // On success there's no other channel back to the caller for this — without logging
            // it here, a successful attach into an app with an empty/unexpected tree would leave
            // no trace of *why*. Goes to the driver's own log stream, not the resolved value.
            if (stderr.trim()) {
                log.info(`attach diagnostics for hwnd ${hwnd}:\n${stderr.trim()}`);
            }
            resolve();
        });
    });
}
