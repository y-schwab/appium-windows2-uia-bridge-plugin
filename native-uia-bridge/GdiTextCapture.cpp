#include "GdiTextCapture.h"
#include "Diagnostics.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace UiaBridge {

namespace {

std::mutex g_mutex;
std::unordered_map<HWND, std::wstring> g_paintedText;

// Every hooked call gets logged, unconditionally, whether or not it yields anything usable — this
// is the decisive answer to "are the hooks even firing, and does WindowFromDC resolve" instead of
// continuing to guess from an empty result. Capped so a long-running attach (hooks stay installed
// for the process's whole lifetime, not just the diagnostic dump at attach time) can't run the log
// file away — plenty to diagnose one attach's worth of repaint activity.
std::atomic<int> g_logCount{0};
constexpr int kMaxLoggedCalls = 200;

void LogHookFire(const wchar_t* fn, HDC hdc, const std::wstring& text) {
    if (g_logCount.fetch_add(1) >= kMaxLoggedCalls) { return; }
    HWND hwnd = WindowFromDC(hdc);
    DiagLog(L"GdiTextCapture: %s hdc=0x%p -> WindowFromDC=0x%p text=\"%s\"", fn, hdc, hwnd, text.c_str());
}

void RecordPaintedText(HDC hdc, const std::wstring& text) {
    // WindowFromDC only resolves for a DC actually tied to a window (a DC obtained via GetDC/
    // GetWindowDC/BeginPaint) — memory/offscreen DCs (e.g. double-buffering) return null here and
    // are silently dropped. FM20 controls in this app are painted directly (confirmed: they have
    // their own real hwnd each, per the diag logs), so this is expected to resolve for them.
    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd || text.empty()) { return; }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_paintedText[hwnd] = text;
}

void RecordPaintedTextA(const wchar_t* fnName, HDC hdc, LPCSTR text, int count) {
    if (!text) { LogHookFire(fnName, hdc, L"<null>"); return; }
    int len = count >= 0 ? count : static_cast<int>(strnlen_s(text, 8192));
    if (len <= 0) { LogHookFire(fnName, hdc, L"<empty>"); return; }
    int wlen = MultiByteToWideChar(CP_ACP, 0, text, len, nullptr, 0);
    if (wlen <= 0) { LogHookFire(fnName, hdc, L"<empty>"); return; }
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, len, wide.data(), wlen);
    LogHookFire(fnName, hdc, wide);
    RecordPaintedText(hdc, wide);
}

void RecordPaintedTextW(const wchar_t* fnName, HDC hdc, LPCWSTR text, int count) {
    if (!text) { LogHookFire(fnName, hdc, L"<null>"); return; }
    size_t len = count >= 0 ? static_cast<size_t>(count) : wcsnlen_s(text, 8192);
    if (len == 0) { LogHookFire(fnName, hdc, L"<empty>"); return; }
    std::wstring wide(text, len);
    LogHookFire(fnName, hdc, wide);
    RecordPaintedText(hdc, wide);
}

// ---- Original function pointers, filled in by InstallGdiTextHooks ----

using TextOutA_t = BOOL(WINAPI*)(HDC, int, int, LPCSTR, int);
using TextOutW_t = BOOL(WINAPI*)(HDC, int, int, LPCWSTR, int);
using ExtTextOutA_t = BOOL(WINAPI*)(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
using ExtTextOutW_t = BOOL(WINAPI*)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
using DrawTextA_t = int(WINAPI*)(HDC, LPCSTR, int, LPRECT, UINT);
using DrawTextW_t = int(WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
using DrawTextExA_t = int(WINAPI*)(HDC, LPSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
using DrawTextExW_t = int(WINAPI*)(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);

TextOutA_t g_origTextOutA = nullptr;
TextOutW_t g_origTextOutW = nullptr;
ExtTextOutA_t g_origExtTextOutA = nullptr;
ExtTextOutW_t g_origExtTextOutW = nullptr;
DrawTextA_t g_origDrawTextA = nullptr;
DrawTextW_t g_origDrawTextW = nullptr;
DrawTextExA_t g_origDrawTextExA = nullptr;
DrawTextExW_t g_origDrawTextExW = nullptr;

// ---- Hook trampolines: capture, then always call through to the real implementation ----

BOOL WINAPI Hook_TextOutA(HDC hdc, int x, int y, LPCSTR text, int count) {
    RecordPaintedTextA(L"TextOutA", hdc, text, count);
    return g_origTextOutA(hdc, x, y, text, count);
}
BOOL WINAPI Hook_TextOutW(HDC hdc, int x, int y, LPCWSTR text, int count) {
    RecordPaintedTextW(L"TextOutW", hdc, text, count);
    return g_origTextOutW(hdc, x, y, text, count);
}
BOOL WINAPI Hook_ExtTextOutA(HDC hdc, int x, int y, UINT options, const RECT* lprc, LPCSTR text, UINT count, const INT* dx) {
    RecordPaintedTextA(L"ExtTextOutA", hdc, text, static_cast<int>(count));
    return g_origExtTextOutA(hdc, x, y, options, lprc, text, count, dx);
}
BOOL WINAPI Hook_ExtTextOutW(HDC hdc, int x, int y, UINT options, const RECT* lprc, LPCWSTR text, UINT count, const INT* dx) {
    RecordPaintedTextW(L"ExtTextOutW", hdc, text, static_cast<int>(count));
    return g_origExtTextOutW(hdc, x, y, options, lprc, text, count, dx);
}
int WINAPI Hook_DrawTextA(HDC hdc, LPCSTR text, int count, LPRECT lprc, UINT format) {
    RecordPaintedTextA(L"DrawTextA", hdc, text, count);
    return g_origDrawTextA(hdc, text, count, lprc, format);
}
int WINAPI Hook_DrawTextW(HDC hdc, LPCWSTR text, int count, LPRECT lprc, UINT format) {
    RecordPaintedTextW(L"DrawTextW", hdc, text, count);
    return g_origDrawTextW(hdc, text, count, lprc, format);
}
int WINAPI Hook_DrawTextExA(HDC hdc, LPSTR text, int count, LPRECT lprc, UINT format, LPDRAWTEXTPARAMS dtp) {
    RecordPaintedTextA(L"DrawTextExA", hdc, text, count);
    return g_origDrawTextExA(hdc, text, count, lprc, format, dtp);
}
int WINAPI Hook_DrawTextExW(HDC hdc, LPWSTR text, int count, LPRECT lprc, UINT format, LPDRAWTEXTPARAMS dtp) {
    RecordPaintedTextW(L"DrawTextExW", hdc, text, count);
    return g_origDrawTextExW(hdc, text, count, lprc, format, dtp);
}

// Walks `module`'s own import descriptors (its IAT, not the exporting DLL's export table) looking
// for an entry named `funcName`. When found, overwrites just that thunk's function pointer with
// `hookFn` and hands back the original address it pointed to — a standard IAT patch, scoped to
// this one importing module so nothing else in the process is affected.
bool PatchImport(HMODULE module, const char* funcName, void* hookFn, void** outOriginal) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return false; }
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { return false; }

    auto& importDataDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDataDir.VirtualAddress == 0) { return false; }

    auto importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDataDir.VirtualAddress);
    for (; importDesc->Name != 0; ++importDesc) {
        auto origThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk));
        auto iatThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) {
                continue; // imported by ordinal, no name to match against
            }
            auto importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(importByName->Name), funcName) != 0) {
                continue;
            }

            void** iatSlot = reinterpret_cast<void**>(&iatThunk->u1.Function);
            DWORD oldProtect = 0;
            if (!VirtualProtect(iatSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                return false;
            }
            if (outOriginal) { *outOriginal = *iatSlot; }
            *iatSlot = hookFn;
            DWORD ignored = 0;
            VirtualProtect(iatSlot, sizeof(void*), oldProtect, &ignored);
            return true;
        }
    }
    return false;
}

} // namespace

bool InstallGdiTextHooks(HMODULE targetModule) {
    if (!targetModule) { return false; }

    struct HookSpec {
        const char* name;
        void* hookFn;
        void** originalSlot;
    };
    HookSpec specs[] = {
        { "TextOutA",     reinterpret_cast<void*>(&Hook_TextOutA),     reinterpret_cast<void**>(&g_origTextOutA) },
        { "TextOutW",     reinterpret_cast<void*>(&Hook_TextOutW),     reinterpret_cast<void**>(&g_origTextOutW) },
        { "ExtTextOutA",  reinterpret_cast<void*>(&Hook_ExtTextOutA),  reinterpret_cast<void**>(&g_origExtTextOutA) },
        { "ExtTextOutW",  reinterpret_cast<void*>(&Hook_ExtTextOutW),  reinterpret_cast<void**>(&g_origExtTextOutW) },
        { "DrawTextA",    reinterpret_cast<void*>(&Hook_DrawTextA),    reinterpret_cast<void**>(&g_origDrawTextA) },
        { "DrawTextW",    reinterpret_cast<void*>(&Hook_DrawTextW),    reinterpret_cast<void**>(&g_origDrawTextW) },
        { "DrawTextExA",  reinterpret_cast<void*>(&Hook_DrawTextExA),  reinterpret_cast<void**>(&g_origDrawTextExA) },
        { "DrawTextExW",  reinterpret_cast<void*>(&Hook_DrawTextExW),  reinterpret_cast<void**>(&g_origDrawTextExW) },
    };

    int patched = 0;
    for (auto& spec : specs) {
        void* original = nullptr;
        if (PatchImport(targetModule, spec.name, spec.hookFn, &original)) {
            *spec.originalSlot = original;
            ++patched;
            DiagLog(L"InstallGdiTextHooks: patched %hs", spec.name);
        }
    }

    DiagLog(L"InstallGdiTextHooks: %d/%zu import(s) patched", patched, ARRAYSIZE(specs));
    return patched > 0;
}

std::wstring GetLastPaintedText(HWND hwnd) {
    if (!hwnd) { return L""; }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_paintedText.find(hwnd);
    return it != g_paintedText.end() ? it->second : L"";
}

} // namespace UiaBridge
