#include "AccessibleTree.h"
#include "Diagnostics.h"
#include "GdiTextCapture.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>

#pragma comment(lib, "oleacc.lib")

namespace UiaBridge {

namespace {

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring RoleToControlType(const VARIANT& roleVariant) {
    if (roleVariant.vt == VT_BSTR && roleVariant.bstrVal) {
        return roleVariant.bstrVal;
    }
    if (roleVariant.vt != VT_I4) {
        return L"Unknown";
    }
    // Approximate MSAA ROLE_SYSTEM_* -> UIA-style ControlType name, covering the roles Forms 2.0
    // controls (CommandButton, TextBox, Label, ComboBox, CheckBox, OptionButton, ListBox, Frame)
    // actually report — not the full MSAA role table.
    switch (roleVariant.lVal) {
        case ROLE_SYSTEM_PUSHBUTTON: return L"Button";
        case ROLE_SYSTEM_TEXT: return L"Edit";
        case ROLE_SYSTEM_STATICTEXT: return L"Text";
        case ROLE_SYSTEM_CHECKBUTTON: return L"CheckBox";
        case ROLE_SYSTEM_RADIOBUTTON: return L"RadioButton";
        case ROLE_SYSTEM_COMBOBOX: return L"ComboBox";
        case ROLE_SYSTEM_LIST: return L"List";
        case ROLE_SYSTEM_LISTITEM: return L"ListItem";
        case ROLE_SYSTEM_GROUPING: return L"Group";
        case ROLE_SYSTEM_PANE: return L"Pane";
        case ROLE_SYSTEM_CLIENT: return L"Pane";
        case ROLE_SYSTEM_WINDOW: return L"Window";
        case ROLE_SYSTEM_MENUITEM: return L"MenuItem";
        case ROLE_SYSTEM_SCROLLBAR: return L"ScrollBar";
        default: {
            wchar_t buf[64];
            if (GetRoleTextW(static_cast<DWORD>(roleVariant.lVal), buf, ARRAYSIZE(buf))) {
                return buf;
            }
            return L"Unknown";
        }
    }
}

// Fallback #1 for GetNodeInfo: bypasses MSAA entirely. Some custom controls (e.g. legacy Win32
// classes with no real IAccessible implementation) still answer plain window text even though
// their accName is empty.
std::wstring TryGetWindowText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) {
        return L"";
    }
    std::wstring buf(static_cast<size_t>(len), L'\0');
    int copied = GetWindowTextW(hwnd, buf.data(), len + 1);
    buf.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return buf;
}

// Fallback #2 for GetNodeInfo: a second, independent WM_GETOBJECT probe against OBJID_WINDOW
// (the window-frame object) rather than OBJID_CLIENT (the content object GetContainerAccessible
// and GetChildren use). Some apps only ever populate the frame object's accName, leaving the
// content object's blank. Safe to SendMessageW here even against our own subclassed root hwnd —
// SubclassProc (WindowSubclass.cpp) only intercepts OBJID_CLIENT/UiaRootObjectId and forwards
// everything else, including OBJID_WINDOW, straight to the original wndproc — so this can't
// recurse into our own provider.
std::wstring TryGetWindowObjectName(HWND hwnd) {
    LRESULT lresult = SendMessageW(hwnd, WM_GETOBJECT, 0, OBJID_WINDOW);
    if (lresult == 0) {
        return L"";
    }
    IAccessible* raw = nullptr;
    if (FAILED(ObjectFromLresult(lresult, IID_IAccessible, 0, reinterpret_cast<void**>(&raw))) || !raw) {
        return L"";
    }
    ComPtr<IAccessible> acc;
    acc.Attach(raw);

    VARIANT self;
    VariantInit(&self);
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;

    std::wstring result;
    BSTR name = nullptr;
    if (SUCCEEDED(acc->get_accName(self, &name)) && name) {
        result.assign(name, SysStringLen(name));
        SysFreeString(name);
    }
    return result;
}

} // namespace

HRESULT GetContainerAccessible(HWND hwnd, ComPtr<IAccessible>& outAcc) {
    if (!IsWindow(hwnd)) {
        return E_INVALIDARG;
    }
    // SendMessageW correctly dispatches to hwnd's owning thread and, since this always runs
    // before our subclass is installed, reaches the container's real original WM_GETOBJECT
    // handler — see the header comment for why this must not be CallWindowProc.
    LRESULT lresult = SendMessageW(hwnd, WM_GETOBJECT, 0, OBJID_CLIENT);
    DiagLog(L"GetContainerAccessible(0x%p): SendMessageW(WM_GETOBJECT, OBJID_CLIENT) -> lresult=%lld", hwnd, static_cast<long long>(lresult));
    if (lresult != 0) {
        IAccessible* raw = nullptr;
        HRESULT unmarshalHr = ObjectFromLresult(lresult, IID_IAccessible, 0, reinterpret_cast<void**>(&raw));
        if (SUCCEEDED(unmarshalHr)) {
            DiagLog(L"GetContainerAccessible(0x%p): primary path (target answered WM_GETOBJECT itself) succeeded", hwnd);
            outAcc.Attach(raw);
            return S_OK;
        }
        DiagLog(L"GetContainerAccessible(0x%p): ObjectFromLresult failed (hr=0x%08lX), trying fallback", hwnd, static_cast<unsigned long>(unmarshalHr));
    } else {
        DiagLog(L"GetContainerAccessible(0x%p): target did not answer WM_GETOBJECT itself (lresult=0) — no app-provided IAccessible, trying fallback", hwnd);
    }

    // Fallback: plenty of legacy Win32 controls (confirmed via Inspect in MSAA mode against this
    // exact target — "Impl: Local oleacc proxy", every provider in the chain a generic Microsoft
    // one, nothing app-specific) never answer WM_GETOBJECT themselves at all. In that case there
    // is no real per-window IAccessible to retrieve — AccessibleObjectFromWindow's own documented
    // fallback (silently synthesizing that same generic proxy object from the hwnd's raw Win32
    // properties, no cooperation from the target required) is the only way to get anything.
    // GetChildren() already relies on this exact API for child hwnds — safe to use here too now
    // that we're running in-process (injected DLL), which sidesteps the out-of-process
    // RPC/marshaling problem that made this function avoid it originally (see this file's header
    // comment) — that problem was specific to controls with *real* custom accessibility, not this
    // proxy-only case.
    IAccessible* raw = nullptr;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void**>(&raw));
    if (FAILED(hr) || !raw) {
        DiagLog(L"GetContainerAccessible(0x%p): fallback AccessibleObjectFromWindow also failed (hr=0x%08lX) — target has no usable IAccessible via any path", hwnd, static_cast<unsigned long>(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }
    DiagLog(L"GetContainerAccessible(0x%p): fallback path (AccessibleObjectFromWindow's generic proxy synthesis) succeeded", hwnd);
    outAcc.Attach(raw);
    return S_OK;
}

namespace {

// Direct (non-recursive) child windows only — EnumChildWindows itself walks the *entire*
// descendant subtree, which would flatten multiple tree levels into one and double-count anything
// reached again once we recurse into each direct child's own GetChildren() call. GetWindow's
// GW_CHILD/GW_HWNDNEXT chain gives just the immediate children, matching how the MSAA side of
// this function only reports one level too.
std::vector<HWND> GetDirectChildWindows(HWND hwnd) {
    std::vector<HWND> result;
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (IsWindowVisible(child)) {
            result.push_back(child);
        }
    }
    return result;
}

} // namespace

std::vector<AccessibleRef> GetChildren(const AccessibleRef& node) {
    std::vector<AccessibleRef> result;
    std::vector<HWND> coveredHwnds; // hwnds already added via the MSAA path, skipped in the window-enum pass below

    if (node.acc && node.childId.vt == VT_I4 && node.childId.lVal == CHILDID_SELF) {
        long childCount = 0;
        if (SUCCEEDED(node.acc->get_accChildCount(&childCount)) && childCount > 0) {
            std::vector<VARIANT> children(childCount);
            long fetched = 0;
            if (SUCCEEDED(AccessibleChildren(node.acc.Get(), 0, childCount, children.data(), &fetched))) {
                for (long i = 0; i < fetched; ++i) {
                    AccessibleRef ref;
                    if (children[i].vt == VT_DISPATCH && children[i].pdispVal) {
                        IAccessible* childAcc = nullptr;
                        if (SUCCEEDED(children[i].pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(&childAcc)))) {
                            ref.acc.Attach(childAcc);
                            ref.childId.vt = VT_I4;
                            ref.childId.lVal = CHILDID_SELF;
                            // Some full child objects are themselves backed by a real window
                            // (e.g. a toolbar control reported as a proper MSAA child object
                            // that also happens to be its own hwnd) — resolving this lets the
                            // window-based walk recurse correctly from this node too, not just
                            // from directly-enumerated child windows.
                            HWND childHwnd = nullptr;
                            if (SUCCEEDED(WindowFromAccessibleObject(ref.acc.Get(), &childHwnd)) && childHwnd) {
                                ref.hwnd = childHwnd;
                                coveredHwnds.push_back(childHwnd);
                            }
                            result.push_back(ref);
                        }
                        VariantClear(&children[i]);
                    } else if (children[i].vt == VT_I4) {
                        // Simple children have no window of their own — nothing to dedupe or recurse into via hwnd.
                        ref.acc = node.acc;
                        ref.childId.vt = VT_I4;
                        ref.childId.lVal = children[i].lVal;
                        result.push_back(ref);
                    }
                }
            }
        }
    }

    // Real child windows this node's own IAccessible never reported — see AccessibleRef's
    // comment on why this second source is necessary. Only applies to nodes we know are backed
    // by a real hwnd (the container itself, or a full child object resolved to one above).
    if (node.hwnd) {
        for (HWND childHwnd : GetDirectChildWindows(node.hwnd)) {
            if (std::find(coveredHwnds.begin(), coveredHwnds.end(), childHwnd) != coveredHwnds.end()) {
                continue;
            }
            IAccessible* childAcc = nullptr;
            if (FAILED(AccessibleObjectFromWindow(childHwnd, OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void**>(&childAcc))) || !childAcc) {
                continue;
            }
            AccessibleRef ref;
            ref.acc.Attach(childAcc);
            ref.childId.vt = VT_I4;
            ref.childId.lVal = CHILDID_SELF;
            ref.hwnd = childHwnd;
            result.push_back(ref);
        }
    }

    return result;
}

AccessibleNodeInfo GetNodeInfo(const AccessibleRef& ref) {
    AccessibleNodeInfo info;

    // OLE-sourced fields (OleControlTree.h) take priority over MSAA's when present — that's the
    // whole point (real Name/Caption/Value/Enabled from the control's own Automation properties,
    // not MSAA's often-thin Forms 2.0 proxy). `haveOleInfo` gates the MSAA assignments below so a
    // root node (which carries both `oleControl` and `acc` — see WindowSubclass.cpp) doesn't have
    // its OLE-sourced fields clobbered by the MSAA pass that still runs for it (root's rect is
    IAccessible* acc = ref.acc.Get();
    if (!acc) {
        return info;
    }

    VARIANT selfId = ref.childId;

    BSTR name = nullptr;
    if (SUCCEEDED(acc->get_accName(selfId, &name)) && name) {
        info.name.assign(name, SysStringLen(name));
        SysFreeString(name);
    }

    // Only ever fills a name MSAA left blank — never overwrites what accName already gave us.
    if (info.name.empty() && ref.hwnd) {
        info.name = TryGetWindowText(ref.hwnd);
    }
    if (info.name.empty() && ref.hwnd) {
        info.name = TryGetWindowObjectName(ref.hwnd);
    }

    VARIANT role;
    VariantInit(&role);
    if (SUCCEEDED(acc->get_accRole(selfId, &role))) {
        info.controlType = RoleToControlType(role);
    }
    VariantClear(&role);

    BSTR value = nullptr;
    if (SUCCEEDED(acc->get_accValue(selfId, &value)) && value) {
        info.value.assign(value, SysStringLen(value));
        SysFreeString(value);
    }

    VARIANT state;
    VariantInit(&state);
    if (SUCCEEDED(acc->get_accState(selfId, &state)) && state.vt == VT_I4) {
        info.isEnabled = (state.lVal & STATE_SYSTEM_UNAVAILABLE) == 0;
    }
    VariantClear(&state);

    // Available via the same IAccessible we already have, but never previously read: exact
    // UIA counterparts (HelpText / AccessKey) exist and are simply unpopulated today.
    BSTR description = nullptr;
    if (SUCCEEDED(acc->get_accDescription(selfId, &description)) && description) {
        info.helpText.assign(description, SysStringLen(description));
        SysFreeString(description);
    }
    BSTR keyboardShortcut = nullptr;
    if (SUCCEEDED(acc->get_accKeyboardShortcut(selfId, &keyboardShortcut)) && keyboardShortcut) {
        info.accessKey.assign(keyboardShortcut, SysStringLen(keyboardShortcut));
        SysFreeString(keyboardShortcut);
    }

    // Last-resort fallback (see NEXT_STEPS.md, path 1): MSAA/window-text gave nothing for this
    // node's name/value (the common case for the F3 Server 60000000 children, whose accessibility
    // plumbing is confirmed broken at the host-app level) — fall back to whatever GDI text was
    // actually painted into this hwnd's DC, captured via GdiTextCapture's IAT hook on FM20.DLL.
    if (ref.hwnd && info.name.empty() && info.value.empty()) {
        std::wstring painted = GetLastPaintedText(ref.hwnd);
        if (!painted.empty()) {
            info.value = painted;
        }
    }

    long x = 0, y = 0, w = 0, h = 0;
    if (SUCCEEDED(acc->accLocation(&x, &y, &w, &h, selfId))) {
        info.rectScreen = { x, y, x + w, y + h };
    }

    // No real automation-id concept in MSAA — see MatchesLocator's docs. Synthesize one from
    // role + name so callers get a stable-ish handle for logging/debugging, not for locating.
    info.automationId = info.controlType + L":" + info.name;

    return info;
}

HRESULT InvokeDefaultAction(IAccessible* acc, const VARIANT& childId) {
    if (!acc) {
        return E_INVALIDARG;
    }
    VARIANT id = childId;
    return acc->accDoDefaultAction(id);
}

HRESULT SetNodeValue(IAccessible* acc, const VARIANT& childId, const std::wstring& value) {
    if (!acc) {
        return E_INVALIDARG;
    }
    VARIANT id = childId;
    BSTR bstr = SysAllocString(value.c_str());
    HRESULT hr = acc->put_accValue(id, bstr);
    SysFreeString(bstr);
    return hr;
}

bool MatchesLocator(const AccessibleNodeInfo& info, LocateStrategy strategy, const std::wstring& value) {
    switch (strategy) {
        case LocateStrategy::Name:
        case LocateStrategy::AccessibilityId:
            return ToLower(info.name) == ToLower(value);
        case LocateStrategy::ClassName:
        case LocateStrategy::ControlType:
            return ToLower(info.controlType) == ToLower(value);
    }
    return false;
}

std::vector<AccessibleRef> FindMatching(
    const AccessibleRef& root,
    LocateStrategy strategy,
    const std::wstring& value,
    bool multiple) {
    std::vector<AccessibleRef> result;

    // BFS to keep result ordering stable/predictable across calls and to bound recursion depth
    // for pathological trees.
    std::vector<AccessibleRef> queue;
    queue.push_back(root);

    for (size_t i = 0; i < queue.size(); ++i) {
        AccessibleNodeInfo info = GetNodeInfo(queue[i]);
        if (MatchesLocator(info, strategy, value)) {
            result.push_back(queue[i]);
            if (!multiple) {
                return result;
            }
        }
        for (auto& child : GetChildren(queue[i])) {
            queue.push_back(child);
        }
    }

    return result;
}

} // namespace UiaBridge
