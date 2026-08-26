#include "WindowSubclass.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <uiautomation.h>

#include "AccessibleTree.h"
#include "Diagnostics.h"
#include "OleControlTree.h"
#include "ProviderRoot.h"

#pragma comment(lib, "UIAutomationCore.lib") // UiaReturnRawElementProvider

namespace UiaBridge {

namespace {

struct SubclassEntry {
    WNDPROC originalProc;
    ProviderRoot* root; // owned — released in RemoveSubclass
};

std::mutex g_mutex;
std::unordered_map<HWND, SubclassEntry> g_entries;

// One-time (per attach), bounded diagnostic dump of what GetChildren() actually finds for the
// root — this is the concrete, per-app answer to "what does this bridge see here at all,"
// combining both the MSAA-logical-children path and the real-child-hwnd path (see GetChildren's
// own comment in AccessibleTree.cpp for why both exist).
void LogDiscoveredChildren(const AccessibleRef& rootRef) {
    long msaaChildCount = -1;
    if (rootRef.acc) {
        rootRef.acc->get_accChildCount(&msaaChildCount);
    }
    auto children = GetChildren(rootRef);
    DiagLog(L"Child discovery for root: MSAA get_accChildCount=%ld, GetChildren() (MSAA children + real child windows, deduplicated) returned %zu total",
        msaaChildCount, children.size());

    size_t shown = 0;
    for (auto& child : children) {
        if (shown >= 30) {
            DiagLog(L"  ... %zu more children not shown (capped at 30)", children.size() - shown);
            break;
        }
        AccessibleNodeInfo info = GetNodeInfo(child);
        wchar_t childClass[256] = {};
        if (child.hwnd) {
            GetClassNameW(child.hwnd, childClass, ARRAYSIZE(childClass));
        }
        DiagLog(L"  [%zu] hwnd=0x%p class=\"%s\" controlType=\"%s\" name=\"%s\" value=\"%s\" rect={l:%ld,t:%ld,r:%ld,b:%ld} enabled=%d",
            shown, child.hwnd, childClass, info.controlType.c_str(), info.name.c_str(), info.value.c_str(),
            info.rectScreen.left, info.rectScreen.top, info.rectScreen.right, info.rectScreen.bottom, info.isEnabled);
        ++shown;
    }
}

LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC original = nullptr;
    ProviderRoot* root = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_entries.find(hwnd);
        if (it != g_entries.end()) {
            original = it->second.originalProc;
            root = it->second.root;
        }
    }

    if (!original) {
        // We were removed (or never installed) concurrently with a message arriving — nothing
        // sane to do but let the default window procedure handle it.
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_GETOBJECT && root != nullptr) {
        if (static_cast<long>(lParam) == static_cast<long>(OBJID_CLIENT) || lParam == UiaRootObjectId) {
            return UiaReturnRawElementProvider(hwnd, wParam, lParam, root);
        }
    }

    if (msg == WM_NCDESTROY) {
        // Tell UI Automation this hwnd's provider is going away before the window is actually
        // gone, then restore the original wndproc and release our provider — matches the
        // cleanup sequence Microsoft's own samples use.
        UiaReturnRawElementProvider(hwnd, 0, 0, nullptr);
        LRESULT result = CallWindowProcW(original, hwnd, msg, wParam, lParam);
        RemoveSubclass(hwnd);
        return result;
    }

    return CallWindowProcW(original, hwnd, msg, wParam, lParam);
}

} // namespace

bool InstallSubclass(HWND hwnd, std::wstring* outError) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_entries.find(hwnd) != g_entries.end()) {
            DiagLog(L"InstallSubclass(0x%p): already subclassed by this DLL", hwnd);
            if (outError) { *outError = L"hwnd is already subclassed by this DLL"; }
            return false;
        }
    }

    WNDPROC originalProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (!originalProc) {
        DiagLog(L"InstallSubclass(0x%p): GetWindowLongPtrW(GWLP_WNDPROC) returned null (error %lu)", hwnd, GetLastError());
        if (outError) { *outError = L"GetWindowLongPtrW(GWLP_WNDPROC) returned null"; }
        return false;
    }

    ComPtr<IAccessible> containerAcc;
    HRESULT containerHr = GetContainerAccessible(hwnd, containerAcc);
    if (FAILED(containerHr) || !containerAcc) {
        // Nothing to bridge — neither the target's own WM_GETOBJECT handler nor
        // AccessibleObjectFromWindow's generic-proxy fallback produced anything (see the
        // GetContainerAccessible-level DiagLog calls just above this in the log for which of the
        // two paths was tried and why each failed).
        DiagLog(L"InstallSubclass(0x%p): GetContainerAccessible failed (hr=0x%08lX) — nothing to bridge", hwnd, static_cast<unsigned long>(containerHr));
        if (outError) {
            wchar_t buf[128];
            swprintf_s(buf, L"GetContainerAccessible failed (hr=0x%08lX) — target's WM_GETOBJECT(OBJID_CLIENT) returned no IAccessible", static_cast<unsigned long>(containerHr));
            *outError = buf;
        }
        return false;
    }

    AccessibleRef rootRef;
    rootRef.acc = containerAcc;
    rootRef.childId.vt = VT_I4;
    rootRef.childId.lVal = CHILDID_SELF;
    rootRef.hwnd = hwnd; // Lets GetChildren() also walk the container's real child windows, not just its own MSAA-reported children.

    // Second, independent discovery source (OleControlTree.h): the Forms 2.0 OLE embedding model,
    // reached via WM_GETOBJECT(OBJID_NATIVEOM) — bypasses MSAA (and its often-thin Forms 2.0 proxy)
    // entirely for these windowless controls. When it succeeds, GetChildren()/GetNodeInfo() prefer
    // it over rootRef.acc's MSAA data (see AccessibleTree.cpp) — rootRef.acc is kept regardless as
    // the fallback for whatever the OLE path doesn't cover. Must also run before the subclass is
    // installed, same reasoning as GetContainerAccessible above.
    ComPtr<IDispatch> rootOleDispatch;
    HRESULT oleHr = GetRootOleDispatch(hwnd, rootOleDispatch);
    if (SUCCEEDED(oleHr) && rootOleDispatch) {
        rootRef.oleControl = rootOleDispatch;
        DiagLog(L"InstallSubclass(0x%p): OLE native-object-model discovery succeeded — preferring it over MSAA for name/value/enabled", hwnd);
    } else {
        DiagLog(L"InstallSubclass(0x%p): OLE native-object-model discovery unavailable (hr=0x%08lX) — using MSAA only", hwnd, static_cast<unsigned long>(oleHr));
    }

    LogDiscoveredChildren(rootRef);

    auto* root = new ProviderRoot(hwnd, rootRef);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_entries[hwnd] = SubclassEntry{ originalProc, root };
    }

    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SubclassProc));
    if (previous == 0 && GetLastError() != 0) {
        DWORD lastError = GetLastError();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_entries.erase(hwnd);
        }
        root->Release();
        DiagLog(L"InstallSubclass(0x%p): SetWindowLongPtrW(GWLP_WNDPROC) failed (error %lu)", hwnd, lastError);
        if (outError) {
            wchar_t buf[96];
            swprintf_s(buf, L"SetWindowLongPtrW(GWLP_WNDPROC) failed (error %lu)", lastError);
            *outError = buf;
        }
        return false;
    }

    DiagLog(L"InstallSubclass(0x%p): subclass installed successfully", hwnd);
    return true;
}

void RemoveSubclass(HWND hwnd) {
    SubclassEntry entry{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_entries.find(hwnd);
        if (it != g_entries.end()) {
            entry = it->second;
            found = true;
            g_entries.erase(it);
        }
    }
    if (!found) {
        return;
    }
    if (IsWindow(hwnd)) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(entry.originalProc));
    }
    entry.root->Release();
}

void RemoveAllSubclasses() {
    std::vector<HWND> hwnds;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& [hwnd, entry] : g_entries) {
            hwnds.push_back(hwnd);
        }
    }
    for (HWND hwnd : hwnds) {
        RemoveSubclass(hwnd);
    }
}

} // namespace UiaBridge
