#include "AccessibleTree.h"
#include "Diagnostics.h"
#include "GdiTextCapture.h"
#include "OleControlTree.h"
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
        case ROLE_SYSTEM_DIALOG: return L"Window"; // UIA has no distinct "Dialog" control type — a dialog is a Window (matches native Inspect against this exact target: ControlType=Window, LocalizedControlType="dialog")
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
    //
    // Deliberately quiet about which of the two paths below actually answered — logging every
    // attempt (not just the winning one) was fine for a single manual attach, but once popups
    // auto-subclass themselves (see WindowSubclass.cpp), the same 3-4 lines repeat per popup and
    // per child inside it, drowning the log. One line at the end, only for the outcome that
    // matters (what GetChildren()/GetNodeInfo actually end up using), is enough to diagnose from.
    LRESULT lresult = SendMessageW(hwnd, WM_GETOBJECT, 0, OBJID_CLIENT);
    if (lresult != 0) {
        IAccessible* raw = nullptr;
        if (SUCCEEDED(ObjectFromLresult(lresult, IID_IAccessible, 0, reinterpret_cast<void**>(&raw)))) {
            outAcc.Attach(raw);
            return S_OK;
        }
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
        DiagLog(L"GetContainerAccessible(0x%p): no usable IAccessible via any path (hr=0x%08lX)", hwnd, static_cast<unsigned long>(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }
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
        if (!IsWindowVisible(child)) { continue; }

        // IsWindowVisible only checks the WS_VISIBLE style bit up the parent chain — it says
        // nothing about actual on-screen area. A classic ComboBox-style control's own dropdown
        // listbox child window is the textbook case: many implementations leave it WS_VISIBLE
        // permanently and just collapse it to zero (or near-zero) size while the dropdown is
        // closed, rather than actually hiding it — that child then passes this filter and shows
        // up in the tree as a real, always-present control that a user can never interact with
        // (confirmed pattern: every ComboBox on this app's forms exposed exactly one such phantom
        // child). Excluding degenerate-size windows here is general (not specific to this app's
        // control library) and self-corrects live: once a dropdown genuinely opens and resizes to
        // its real on-screen extent, the next tree walk picks it up normally, since this function
        // queries the real window state every call rather than caching anything.
        RECT rect{};
        if (GetWindowRect(child, &rect) && (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0)) {
            continue; // zero/negative area — see the comment above; GetWindowRect failure falls through and keeps the window rather than guessing
        }

        result.push_back(child);
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
                            bool zeroArea = false;
                            if (SUCCEEDED(WindowFromAccessibleObject(ref.acc.Get(), &childHwnd)) && childHwnd) {
                                ref.hwnd = childHwnd;
                                coveredHwnds.push_back(childHwnd);
                                // Same zero-area exclusion as the window-enum path below, applied
                                // here too — a ComboBox-style control's closed dropdown-list child
                                // is just as likely to arrive as a full MSAA child object resolved
                                // to its own hwnd (this branch) as via plain window enumeration,
                                // and a zero-size element is equally useless either way: it can
                                // never be clicked, located by point, or usefully inspected. Only
                                // excluded here, not from `result` on the simple-child branch below
                                // (those have no hwnd/rect of their own to check at all).
                                RECT rect{};
                                zeroArea = GetWindowRect(childHwnd, &rect) && (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0);
                            }
                            if (!zeroArea) {
                                result.push_back(ref);
                            }
                        }
                        VariantClear(&children[i]); // must run regardless of zeroArea — releases AccessibleChildren's own reference on children[i].pdispVal
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

    // Tier 2 (see NEXT_STEPS.md's extraction-tier table): MSAA/window-text (tiers 0-1) gave
    // nothing for this node — try the OLE embedding model / native-object-model next, before
    // falling all the way to GDI paint capture. Genuinely optional per app (this was a dead end
    // for the one target app that originally motivated this codebase, but that's one app's
    // accessibility plumbing being broken, not evidence the whole approach is dead) — every read
    // is individually best-effort (OleControlTree.cpp SEH-guards the actual COM calls), so a
    // target with no OLE support at all just falls through to tier 3/4 exactly as it did before
    // this tier existed.
    if (ref.hwnd && info.name.empty() && info.value.empty()) {
        ComPtr<IDispatch> ole;
        if (SUCCEEDED(GetRootOleDispatch(ref.hwnd, ole)) && ole) {
            OleControlInfo oi = GetOleControlInfo(ole.Get());
            if (!oi.name.empty()) { info.name = oi.name; }
            if (info.value.empty() && !oi.text.empty()) { info.value = oi.text; }
            if (info.helpText.empty() && !oi.helpText.empty()) { info.helpText = oi.helpText; }
            if (info.accessKey.empty() && !oi.accessKey.empty()) { info.accessKey = oi.accessKey; }
            if (!oi.name.empty() || !oi.text.empty()) {
                DiagLog(L"GetNodeInfo(0x%p): tier 2 (OLE native-object-model) supplied name=\"%s\" value=\"%s\"",
                    ref.hwnd, info.name.c_str(), info.value.c_str());
            }
        }
    }

    // Tier 3/4 (see NEXT_STEPS.md): still nothing for this node's name/value — fall back to
    // whatever GDI text was actually painted into this hwnd's DC, captured via GdiTextCapture's
    // IAT hook across every loaded module.
    // GetLastPaintedText only has something to report once a paint has actually happened through
    // a hooked call since the hook was installed — these controls draw themselves once at
    // window-open and don't repaint on their own, so the capture map would otherwise stay empty
    // forever. Force one here: InvalidateRect+UpdateWindow, called cross-thread like this,
    // dispatches WM_PAINT directly to the target's own wndproc (the same SendMessage-style
    // blocking semantics WM_GETOBJECT already relies on elsewhere in this codebase) — synchronous,
    // so the hooked GDI calls have already run by the time UpdateWindow returns.
    if (ref.hwnd && info.name.empty() && info.value.empty()) {
        InvalidateRect(ref.hwnd, nullptr, TRUE);
        UpdateWindow(ref.hwnd);
        std::wstring painted = GetLastPaintedText(ref.hwnd);
        // Quiet on failure — an empty result here is the common case for every node that isn't a
        // GDI-painted control, not something worth a log line each time (same reasoning as
        // GetContainerAccessible above). LogDiscoveredChildren already prints the final
        // name/value per child either way, which is the one line that actually matters.
        if (!painted.empty()) {
            DiagLog(L"GetNodeInfo(0x%p): tier 3/4 (GDI paint capture) supplied \"%s\"", ref.hwnd, painted.c_str());
            // These captured strings are almost always button/label captions (that's what's
            // actually painted for the F3 Server children) — UIA convention is that a caption is
            // the control's Name, not its Value (Value pattern is for editable text, and real
            // Win32 buttons don't implement it at all). Populate both: Name so Inspect's basic
            // property list, screen readers, and Appium's own name/text lookups all see it
            // immediately; Value too, since it costs nothing and some callers check there instead.
            info.name = painted;
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
