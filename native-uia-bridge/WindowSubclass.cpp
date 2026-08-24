#include "WindowSubclass.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <uiautomation.h>

#include "AccessibleTree.h"
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

bool InstallSubclass(HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_entries.find(hwnd) != g_entries.end()) {
            return false; // Already subclassed.
        }
    }

    WNDPROC originalProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (!originalProc) {
        return false;
    }

    ComPtr<IAccessible> containerAcc;
    if (FAILED(GetContainerAccessible(hwnd, containerAcc)) || !containerAcc) {
        return false; // Nothing to bridge — the container answered WM_GETOBJECT with no IAccessible.
    }

    AccessibleRef rootRef;
    rootRef.acc = containerAcc;
    rootRef.childId.vt = VT_I4;
    rootRef.childId.lVal = CHILDID_SELF;
    rootRef.hwnd = hwnd; // Lets GetChildren() also walk the container's real child windows, not just its own MSAA-reported children.

    auto* root = new ProviderRoot(hwnd, rootRef);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_entries[hwnd] = SubclassEntry{ originalProc, root };
    }

    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SubclassProc));
    if (previous == 0 && GetLastError() != 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_entries.erase(hwnd);
        root->Release();
        return false;
    }

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
