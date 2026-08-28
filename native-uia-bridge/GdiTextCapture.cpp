#include "GdiTextCapture.h"
#include "Diagnostics.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <tlhelp32.h>
#include <unordered_map>
#include <vector>

namespace UiaBridge {

namespace {

std::mutex g_mutex;
std::unordered_map<HWND, std::wstring> g_paintedText;

// GDI+ (GdipDrawString, GDIPLUS.DLL) is a completely separate text-drawing API from classic GDI —
// confirmed loaded in this process (see NEXT_STEPS.md's module dump), and the app's buttons have
// the icon+gradient look typical of GDI+ rendering. A GpGraphics* carries no reference back to the
// HDC/HWND it draws into, unlike an HDC (WindowFromDC), so it has to be tracked ourselves: hook
// GdipCreateFromHDC/GdipCreateFromHWND to remember which hwnd each GpGraphics* belongs to, and
// GdipDeleteGraphics to stop tracking it once freed.
std::unordered_map<void*, HWND> g_graphicsToHwnd;

// Every hooked call gets logged, unconditionally, whether or not it yields anything usable — this
// is the decisive answer to "are the hooks even firing, and does WindowFromDC resolve" instead of
// continuing to guess from an empty result. Capped so a long-running attach (hooks stay installed
// for the process's whole lifetime, not just the diagnostic dump at attach time) can't run the log
// file away — plenty to diagnose one attach's worth of repaint activity.
std::atomic<int> g_logCount{0};
constexpr int kMaxLoggedCalls = 200;

// Tier 3 generalization: derive the codepage for a captured ANSI/zero-extended string from the
// *actual font selected in this hdc*, via GetTextCharsetInfo, instead of a hardcoded script
// (originally hardcoded to CP1255/Windows-Hebrew for the one app that motivated this codebase —
// wrong for anything else, e.g. an Arabic or Cyrillic app would need CP1256/CP1251 instead). Best
// effort throughout: any failure (charset lookup fails, reported codepage isn't installed) falls
// back to CP_ACP rather than erroring, so a target this heuristic doesn't fit for just gets
// whatever the classic (imperfect) behavior always was, never a crash or a dropped capture.
UINT CodePageForHdc(HDC hdc) {
    if (!hdc) { return CP_ACP; }
    int charset = GetTextCharset(hdc);
    if (charset == DEFAULT_CHARSET || charset == 0) { return CP_ACP; }
    CHARSETINFO csi{};
    if (TranslateCharsetInfo(reinterpret_cast<DWORD*>(static_cast<INT_PTR>(charset)), &csi, TCI_SRCCHARSET) && IsValidCodePage(csi.ciACP)) {
        return csi.ciACP;
    }
    return CP_ACP;
}

// A "wide" string captured via one of the *W hooks can still turn out to be legacy ANSI text that
// got zero-extended into wchar_t one byte at a time instead of properly converted (confirmed via
// real-device capture: DlgLibrary.dll's ExtTextOutW calls came through as Latin-1-range garbage —
// every code point < 0x100 — for text known to actually be Hebrew). Seeing every code point stay
// under 0x100 is the general tell (true for any single-byte-codepage script, not just Hebrew) that
// each wchar_t is really just a raw codepage byte value in disguise. Reinterpreting via
// CodePageForHdc is a safe no-op for genuinely ASCII text (every single-byte Windows codepage
// agrees with ASCII for 0..127), so this is applied unconditionally whenever the heuristic matches
// rather than special-casing which hook or which script needs it.
std::wstring FixupMisencodedAnsiText(HDC hdc, const std::wstring& text) {
    if (text.empty()) { return text; }
    for (wchar_t c : text) {
        if (c > 0xFF) { return text; } // has real non-Latin-1 code points -> already correct Unicode
    }
    std::string bytes(text.size(), '\0');
    for (size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<char>(text[i] & 0xFF);
    }
    UINT cp = CodePageForHdc(hdc);
    int wlen = MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wlen <= 0) { return text; }
    std::wstring fixed(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), fixed.data(), wlen);
    return fixed;
}

// ETO_GLYPHINDEX (0x0010, wingdi.h): when set on an ExtTextOut* call, `text` isn't characters at
// all — it's raw glyph indices into the currently-selected font, an optimization apps use to skip
// character-to-glyph mapping (common for precomputed/shaped complex-script runs — Hebrew, Arabic,
// or any script an app chose to pre-shape). If that's what's happening, no codepage
// reinterpretation of the "text" will ever produce real characters, because it was never character
// data to begin with.
constexpr UINT kEtoGlyphIndex = 0x0010;

// ETO_RTLREADING (0x0080, wingdi.h): GDI's own signal that this run should be laid out right-to-
// left. Confirmed (real-device diag) that RTL glyph runs come out in *visual* order (left-to-right
// on screen), not *logical* (reading) order — a straightforward per-glyph decode of an RTL Hebrew
// caption came back mirrored (e.g. "האיצי" for what's actually "יציאה"/Exit) until reversed. Using
// this flag instead of unconditionally reversing (or hardcoding "this app is Hebrew, always
// reverse") makes the fix general: an LTR run some other app draws via ETO_GLYPHINDEX is left
// alone. A genuinely mixed-direction single line would need real BiDi-run handling instead of a
// flat reversal, but that's a rarer case than getting single-direction scripts right generally.
constexpr UINT kEtoRtlReading = 0x0080;

// There's no built-in "glyph index -> character" API — GetGlyphIndicesW only goes the other way
// (character -> glyph, for the currently-selected font) — so the reverse map has to be built by
// brute force: run a candidate set of characters through GetGlyphIndicesW against the same
// HDC/font the real call used, and invert the result. Cached per HFONT so this only runs once per
// distinct font actually seen, not on every captured string.
//
// Tier 4 generalization: the candidate set now comes from GetFontUnicodeRanges — the font's own
// declared coverage — instead of a hardcoded Hebrew block, so this works for whatever script a
// future target app actually uses (Arabic, Cyrillic, Thai, a CJK subset within the cap below...)
// without new code. Capped so a CJK-scale font (tens of thousands of glyphs) can't turn the first
// paint of a new font into a multi-second stall; a partial candidate set still decodes whatever
// falls inside the cap; nothing beyond it. Falls back to a small ASCII+Latin-1 set if the font
// reports no ranges at all (e.g. a raster font that doesn't support the call) — best effort, never
// an empty/failed decode where a fallback is possible.
constexpr UINT kMaxGlyphCandidates = 6000;

std::vector<wchar_t> FallbackCandidateChars() {
    std::vector<wchar_t> chars;
    for (wchar_t c = 0x0020; c <= 0x00FF; ++c) { chars.push_back(c); } // ASCII + Latin-1 supplement
    return chars;
}

std::vector<wchar_t> BuildCandidateCharsForHdc(HDC hdc) {
    DWORD size = GetFontUnicodeRanges(hdc, nullptr);
    if (size == 0) { return FallbackCandidateChars(); }

    std::vector<BYTE> buffer(size);
    auto* glyphSet = reinterpret_cast<GLYPHSET*>(buffer.data());
    if (GetFontUnicodeRanges(hdc, glyphSet) == 0 || glyphSet->cRanges == 0) {
        return FallbackCandidateChars();
    }

    std::vector<wchar_t> chars;
    chars.reserve(std::min<UINT>(glyphSet->cGlyphsSupported, kMaxGlyphCandidates));
    for (DWORD r = 0; r < glyphSet->cRanges && chars.size() < kMaxGlyphCandidates; ++r) {
        WCHAR low = glyphSet->ranges[r].wcLow;
        USHORT count = glyphSet->ranges[r].cGlyphs;
        for (USHORT j = 0; j < count && chars.size() < kMaxGlyphCandidates; ++j) {
            chars.push_back(static_cast<wchar_t>(low + j));
        }
    }
    return chars.empty() ? FallbackCandidateChars() : chars;
}

std::mutex g_glyphMapMutex;
std::unordered_map<HFONT, std::unordered_map<WORD, wchar_t>> g_glyphMaps;

const std::unordered_map<WORD, wchar_t>* GetOrBuildGlyphMap(HDC hdc) {
    HFONT font = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
    if (!font) { return nullptr; }

    {
        std::lock_guard<std::mutex> lock(g_glyphMapMutex);
        auto it = g_glyphMaps.find(font);
        if (it != g_glyphMaps.end()) { return &it->second; }
    }

    std::unordered_map<WORD, wchar_t> map;
    for (wchar_t ch : BuildCandidateCharsForHdc(hdc)) {
        WORD glyph = 0xFFFF;
        DWORD result = GetGlyphIndicesW(hdc, &ch, 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS);
        if (result != GDI_ERROR && glyph != 0xFFFF) {
            map.emplace(glyph, ch); // first candidate wins a colliding glyph index — good enough for capture purposes
        }
    }

    std::lock_guard<std::mutex> lock(g_glyphMapMutex);
    auto [it, inserted] = g_glyphMaps.emplace(font, std::move(map));
    return &it->second;
}

// Best-effort decode of a captured glyph-index run back into characters, using the font actually
// selected in `hdc` at call time. A glyph with no match in the candidate set becomes '?' rather
// than silently dropped, so a partial/wrong-candidate-set decode is still visibly a decode, not
// mistaken for a clean one.
std::wstring DecodeGlyphIndices(HDC hdc, const wchar_t* glyphData, size_t count, UINT options) {
    const auto* map = GetOrBuildGlyphMap(hdc);
    if (!map) { return L""; }
    std::wstring result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        WORD glyph = static_cast<WORD>(glyphData[i]);
        auto it = map->find(glyph);
        result.push_back(it != map->end() ? it->second : L'?');
    }
    if (options & kEtoRtlReading) {
        std::reverse(result.begin(), result.end());
    }
    return result;
}

void LogHookFire(const wchar_t* fn, HDC hdc, UINT options, const std::wstring& text) {
    if (g_logCount.fetch_add(1) >= kMaxLoggedCalls) { return; }
    HWND hwnd = WindowFromDC(hdc);
    DiagLog(L"GdiTextCapture: %s hdc=0x%p options=0x%X%s -> WindowFromDC=0x%p text=\"%s\"",
        fn, hdc, options, (options & kEtoGlyphIndex) ? L" (ETO_GLYPHINDEX!)" : L"", hwnd, text.c_str());
}

void RecordPaintedTextForHwnd(HWND hwnd, const std::wstring& text) {
    if (!hwnd || text.empty()) { return; }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_paintedText[hwnd] = text;
}

void RecordPaintedText(HDC hdc, const std::wstring& text) {
    // WindowFromDC only resolves for a DC actually tied to a window (a DC obtained via GetDC/
    // GetWindowDC/BeginPaint) — memory/offscreen DCs (e.g. double-buffering) return null here and
    // are silently dropped. FM20 controls in this app are painted directly (confirmed: they have
    // their own real hwnd each, per the diag logs), so this is expected to resolve for them.
    RecordPaintedTextForHwnd(WindowFromDC(hdc), text);
}

void RecordPaintedTextA(const wchar_t* fnName, HDC hdc, LPCSTR text, int count, UINT options = 0) {
    if (!text) { LogHookFire(fnName, hdc, options, L"<null>"); return; }
    int len = count >= 0 ? count : static_cast<int>(strnlen_s(text, 8192));
    if (len <= 0) { LogHookFire(fnName, hdc, options, L"<empty>"); return; }
    if (options & kEtoGlyphIndex) {
        // Not real character data — logging it as hex is more honest than pretending a codepage
        // conversion means anything here.
        wchar_t hexBuf[512] = {};
        size_t pos = 0;
        for (int i = 0; i < len && pos + 3 < ARRAYSIZE(hexBuf); ++i) {
            pos += swprintf_s(hexBuf + pos, ARRAYSIZE(hexBuf) - pos, L"%02X ", static_cast<unsigned char>(text[i]));
        }
        LogHookFire(fnName, hdc, options, hexBuf);
        return;
    }
    UINT cp = CodePageForHdc(hdc); // the *A hooks' text is this app's own ANSI data in whatever codepage its font implies, not necessarily CP_ACP — see CodePageForHdc's comment
    int wlen = MultiByteToWideChar(cp, 0, text, len, nullptr, 0);
    if (wlen <= 0) { LogHookFire(fnName, hdc, options, L"<empty>"); return; }
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(cp, 0, text, len, wide.data(), wlen);
    LogHookFire(fnName, hdc, options, wide);
    RecordPaintedText(hdc, wide);
}

void RecordPaintedTextW(const wchar_t* fnName, HDC hdc, LPCWSTR text, int count, UINT options = 0) {
    if (!text) { LogHookFire(fnName, hdc, options, L"<null>"); return; }
    size_t len = count >= 0 ? static_cast<size_t>(count) : wcsnlen_s(text, 8192);
    if (len == 0) { LogHookFire(fnName, hdc, options, L"<empty>"); return; }
    if (options & kEtoGlyphIndex) {
        std::wstring decoded = DecodeGlyphIndices(hdc, text, len, options);
        wchar_t hexBuf[512] = {};
        size_t pos = 0;
        for (size_t i = 0; i < len && pos + 6 < ARRAYSIZE(hexBuf); ++i) {
            pos += swprintf_s(hexBuf + pos, ARRAYSIZE(hexBuf) - pos, L"%04X ", static_cast<unsigned short>(text[i]));
        }
        if (!decoded.empty()) {
            LogHookFire(fnName, hdc, options, decoded + L"  [glyphs: " + hexBuf + L"]");
            RecordPaintedText(hdc, decoded);
        } else {
            LogHookFire(fnName, hdc, options, hexBuf);
        }
        return;
    }
    std::wstring wide = FixupMisencodedAnsiText(hdc, std::wstring(text, len));
    LogHookFire(fnName, hdc, options, wide);
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

// GDI+ flat C API (gdiplusflat.h) — GpGraphics/GpFont/RectF/GpStringFormat/GpBrush are all opaque
// from here, so every pointer parameter is declared void*; that's ABI-identical to the real
// pointer types (only the pointee's type differs, not how it's passed), so this is safe without
// pulling in the actual GDI+ headers. GpStatus is an int-sized enum (0 == Ok).
using GdipCreateFromHDC_t = int(WINAPI*)(HDC, void**);
using GdipCreateFromHWND_t = int(WINAPI*)(HWND, void**);
using GdipDeleteGraphics_t = int(WINAPI*)(void*);
using GdipDrawString_t = int(WINAPI*)(void*, LPCWSTR, int, const void*, const void*, const void*, const void*);

GdipCreateFromHDC_t g_origGdipCreateFromHDC = nullptr;
GdipCreateFromHWND_t g_origGdipCreateFromHWND = nullptr;
GdipDeleteGraphics_t g_origGdipDeleteGraphics = nullptr;
GdipDrawString_t g_origGdipDrawString = nullptr;

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
    RecordPaintedTextA(L"ExtTextOutA", hdc, text, static_cast<int>(count), options);
    return g_origExtTextOutA(hdc, x, y, options, lprc, text, count, dx);
}
BOOL WINAPI Hook_ExtTextOutW(HDC hdc, int x, int y, UINT options, const RECT* lprc, LPCWSTR text, UINT count, const INT* dx) {
    RecordPaintedTextW(L"ExtTextOutW", hdc, text, static_cast<int>(count), options);
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

int WINAPI Hook_GdipCreateFromHDC(HDC hdc, void** graphics) {
    int status = g_origGdipCreateFromHDC(hdc, graphics);
    if (status == 0 && graphics && *graphics) {
        HWND hwnd = WindowFromDC(hdc);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_graphicsToHwnd[*graphics] = hwnd;
    }
    return status;
}
int WINAPI Hook_GdipCreateFromHWND(HWND hwnd, void** graphics) {
    int status = g_origGdipCreateFromHWND(hwnd, graphics);
    if (status == 0 && graphics && *graphics) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_graphicsToHwnd[*graphics] = hwnd;
    }
    return status;
}
int WINAPI Hook_GdipDeleteGraphics(void* graphics) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_graphicsToHwnd.erase(graphics);
    }
    return g_origGdipDeleteGraphics(graphics);
}
int WINAPI Hook_GdipDrawString(void* graphics, LPCWSTR string, int length, const void* font, const void* layoutRect, const void* stringFormat, const void* brush) {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_graphicsToHwnd.find(graphics);
        if (it != g_graphicsToHwnd.end()) { hwnd = it->second; }
    }
    if (string) {
        size_t len = length >= 0 ? static_cast<size_t>(length) : wcsnlen_s(string, 8192);
        // No HDC available here — GDI+'s Graphics object wraps its own internal DC, not exposed
        // through GdipDrawString's own parameters — so CodePageForHdc(nullptr) falls back to
        // CP_ACP for this path specifically; best effort, same as every other fallback in here.
        std::wstring text = FixupMisencodedAnsiText(nullptr, std::wstring(string, len));
        if (g_logCount.fetch_add(1) < kMaxLoggedCalls) {
            DiagLog(L"GdiTextCapture: GdipDrawString graphics=0x%p -> hwnd=0x%p text=\"%s\"", graphics, hwnd, text.c_str());
        }
        RecordPaintedTextForHwnd(hwnd, text);
    }
    return g_origGdipDrawString(graphics, string, length, font, layoutRect, stringFormat, brush);
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
        // GDI+ (see the g_graphicsToHwnd comment above) — same IAT-patch mechanism, just against
        // whichever import table entries FM20.DLL has for GDIPLUS.DLL's flat C API instead of
        // gdi32's classic one.
        { "GdipCreateFromHDC",  reinterpret_cast<void*>(&Hook_GdipCreateFromHDC),  reinterpret_cast<void**>(&g_origGdipCreateFromHDC) },
        { "GdipCreateFromHWND", reinterpret_cast<void*>(&Hook_GdipCreateFromHWND), reinterpret_cast<void**>(&g_origGdipCreateFromHWND) },
        { "GdipDeleteGraphics", reinterpret_cast<void*>(&Hook_GdipDeleteGraphics), reinterpret_cast<void**>(&g_origGdipDeleteGraphics) },
        { "GdipDrawString",     reinterpret_cast<void*>(&Hook_GdipDrawString),     reinterpret_cast<void**>(&g_origGdipDrawString) },
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

    // Only log the summary when this module actually had something — InstallGdiTextHooksEverywhere
    // calls this against every loaded module, most of which import none of these, and a "0/12"
    // line per module would drown the log in noise for no signal.
    if (patched > 0) {
        DiagLog(L"InstallGdiTextHooks: %d/%zu import(s) patched", patched, ARRAYSIZE(specs));
    }
    return patched > 0;
}

int InstallGdiTextHooksEverywhere() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        DiagLog(L"InstallGdiTextHooksEverywhere: CreateToolhelp32Snapshot failed (error %lu)", GetLastError());
        return 0;
    }

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&InstallGdiTextHooksEverywhere), &self);

    int modulesPatched = 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (entry.hModule == self) { continue; } // no reason to patch our own DLL's imports
            if (InstallGdiTextHooks(entry.hModule)) {
                ++modulesPatched;
                DiagLog(L"InstallGdiTextHooksEverywhere: %s had at least one matching import", entry.szModule);
            }
        } while (Module32NextW(snapshot, &entry));
    } else {
        DiagLog(L"InstallGdiTextHooksEverywhere: Module32FirstW failed (error %lu)", GetLastError());
    }
    CloseHandle(snapshot);

    DiagLog(L"InstallGdiTextHooksEverywhere: %d module(s) had a matching import patched", modulesPatched);
    return modulesPatched;
}

std::wstring GetLastPaintedText(HWND hwnd) {
    if (!hwnd) { return L""; }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_paintedText.find(hwnd);
    return it != g_paintedText.end() ? it->second : L"";
}

} // namespace UiaBridge
