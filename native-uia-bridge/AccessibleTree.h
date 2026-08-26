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

// One node in the container's accessible tree — either the container itself (childId =
// CHILDID_SELF, acc = the container's own IAccessible), a "simple child" (acc = parent's
// IAccessible, childId = a plain integer with no IDispatch of its own), or a full child object
// (acc = the child's own IAccessible, childId = CHILDID_SELF) — MSAA allows either shape per
// child. `hwnd` is set whenever this node is backed by a real Win32 window (resolved via
// WindowFromAccessibleObject for MSAA-reported full objects, or known directly for nodes
// discovered by walking child windows) — GetChildren() uses it to also enumerate that window's
// *real* child windows, not just whatever the parent's own IAccessible chooses to report as
// children. A window's own MSAA child enumeration frequently only covers its directly-painted
// content, not other genuine child HWNDs nested inside it (e.g. a toolbar or button that is its
// own separate window) — those need this second, HWND-based discovery path or they're invisible
// to the tree entirely, even though they usually carry far richer name/role/text than the
// container's own thin surface.
struct AccessibleRef {
    ComPtr<IAccessible> acc;
    VARIANT childId{};
    HWND hwnd = nullptr;

    // Set instead of (root) or in addition to (root only — see OleControlTree.h) `acc` when this
    // node was discovered via the OLE-embedding / native-object-model path (OleControlTree.h)
    // rather than MSAA. `oleParentScreenRect` is this node's immediate parent's already-resolved
    // screen rect, needed to convert the control's own Left/Top (points, parent-relative) into
    // screen coordinates — see GetNodeInfo's oleControl branch in AccessibleTree.cpp.
    ComPtr<IDispatch> oleControl;
    RECT oleParentScreenRect{};

    AccessibleRef() { VariantInit(&childId); }
    AccessibleRef(const AccessibleRef& other)
        : acc(other.acc), hwnd(other.hwnd), oleControl(other.oleControl), oleParentScreenRect(other.oleParentScreenRect) {
        VariantInit(&childId);
        VariantCopy(&childId, const_cast<VARIANT*>(&other.childId));
    }
    AccessibleRef& operator=(const AccessibleRef& other) {
        if (this != &other) {
            acc = other.acc;
            hwnd = other.hwnd;
            oleControl = other.oleControl;
            oleParentScreenRect = other.oleParentScreenRect;
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
//
// Falls back to AccessibleObjectFromWindow if the target never answers WM_GETOBJECT itself — see
// this function's own comment in the .cpp for why that's safe here despite the out-of-process
// marshaling problem that originally motivated avoiding that API.
HRESULT GetContainerAccessible(HWND hwnd, ComPtr<IAccessible>& outAcc);

// Enumerates the direct children of `node` — both the MSAA-reported logical children
// (AccessibleChildren) and, when `node.hwnd` is set, that window's real direct child windows
// (each queried independently via AccessibleObjectFromWindow) — deduplicated against each other
// by hwnd. See AccessibleRef's comment for why both sources are needed.
std::vector<AccessibleRef> GetChildren(const AccessibleRef& node);

// `ref.hwnd`, when set, unlocks two fallback sources tried in order whenever the MSAA accName
// comes back empty: GetWindowTextW on the hwnd directly (bypasses MSAA entirely — some custom
// controls answer window text even with broken/absent accessibility), then a second WM_GETOBJECT
// probe against OBJID_WINDOW instead of OBJID_CLIENT (some apps only populate the window-frame
// object's name, leaving the content object blank). Both are pure reads with no side effect on
// the target; neither ever overwrites a name MSAA already gave us.
AccessibleNodeInfo GetNodeInfo(const AccessibleRef& ref);

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

// Breadth-first search starting at `root`. If `multiple` is false, stops at the first match.
std::vector<AccessibleRef> FindMatching(
    const AccessibleRef& root,
    LocateStrategy strategy,
    const std::wstring& value,
    bool multiple);

} // namespace UiaBridge
