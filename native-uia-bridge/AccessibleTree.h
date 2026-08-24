#pragma once
// In-process MSAA (IAccessible) tree walking for the Forms 2.0 / F3 Server container. Because
// this code runs *inside* the target process (loaded via LoadLibrary, not called cross-process),
// calling the container's IAccessible directly here never goes through the RPC/marshaling layer
// that makes out-of-process AccessibleObjectFromWindow calls return [null] for these controls —
// it's a same-apartment vtable call, exactly like the app's own code would make.

#include <oleacc.h>
#include <string>
#include <vector>
#include <windows.h>

#include "ComPtr.h"

namespace UiaBridge {

// One node in the container's MSAA tree — either the container itself (childId = CHILDID_SELF,
// acc = the container's own IAccessible) or a "simple child" (acc = parent's IAccessible,
// childId = a plain integer with no IDispatch of its own) or a full child object (acc = the
// child's own IAccessible, childId = CHILDID_SELF) — MSAA allows either shape per child.
struct AccessibleRef {
    ComPtr<IAccessible> acc;
    VARIANT childId{};

    AccessibleRef() { VariantInit(&childId); }
    AccessibleRef(const AccessibleRef& other) : acc(other.acc) {
        VariantInit(&childId);
        VariantCopy(&childId, const_cast<VARIANT*>(&other.childId));
    }
    AccessibleRef& operator=(const AccessibleRef& other) {
        if (this != &other) {
            acc = other.acc;
            VariantClear(&childId);
            VariantCopy(&childId, const_cast<VARIANT*>(&other.childId));
        }
        return *this;
    }
    ~AccessibleRef() { VariantClear(&childId); }
};

struct AccessibleNodeInfo {
    std::wstring name;
    std::wstring controlType; // approximate UIA-style control type name, mapped from the MSAA role
    std::wstring automationId; // synthetic — see FindMatching()'s "accessibility id" note
    std::wstring value;
    bool isEnabled = true;
    RECT rectScreen{};
};

// Sends WM_GETOBJECT(OBJID_CLIENT) to `hwnd` and resolves the returned LRESULT into a real
// IAccessible via ObjectFromLresult. Must be called BEFORE our subclass is installed (see
// WindowSubclass.cpp's InstallSubclass, the only caller) — at that point the hwnd's current
// wndproc is still the container's own original one, so SendMessageW is safe: it dispatches to
// the window's real owning thread (unlike CallWindowProc, which would run the target's code on
// whichever thread calls it — a same-apartment/thread-affinity hazard for arbitrary Win32/COM
// UI code). Once the subclass IS installed, WM_GETOBJECT must never be sent via SendMessage from
// inside our own handler, since that would recurse into ourselves — but that concern doesn't
// apply here, since this call always happens first.
HRESULT GetContainerAccessible(HWND hwnd, ComPtr<IAccessible>& outAcc);

// Enumerates the direct children of `parent`/`parentChildId` (pass CHILDID_SELF for the
// container's own top-level children).
std::vector<AccessibleRef> GetChildren(IAccessible* parent, const VARIANT& parentChildId);

AccessibleNodeInfo GetNodeInfo(IAccessible* acc, const VARIANT& childId);

// Invokes the node's default action (IAccessible::accDoDefaultAction) — the closest MSAA
// equivalent of UIA's IInvokeProvider::Invoke.
HRESULT InvokeDefaultAction(IAccessible* acc, const VARIANT& childId);

HRESULT SetNodeValue(IAccessible* acc, const VARIANT& childId, const std::wstring& value);

enum class LocateStrategy { Name, AccessibilityId, ClassName, ControlType };

// `AccessibilityId` and `ClassName` fall back to matching against Name/controlType respectively —
// classic MSAA (unlike UIA) has no first-class AutomationId or ClassName concept, so this is a
// documented approximation, not a bug: Forms 2.0 controls rarely expose anything better via
// IAccessible. A future iteration could try IDispatch::Invoke for a control-specific "Tag"/"Name"
// property where the underlying control supports late binding, but that's per-control-type work.
bool MatchesLocator(const AccessibleNodeInfo& info, LocateStrategy strategy, const std::wstring& value);

// Depth-first search starting at `root`/`rootChildId`. If `multiple` is false, stops at the
// first match.
std::vector<AccessibleRef> FindMatching(
    IAccessible* root,
    const VARIANT& rootChildId,
    LocateStrategy strategy,
    const std::wstring& value,
    bool multiple);

} // namespace UiaBridge
