#include "AccessibleTree.h"
#include <algorithm>
#include <cctype>
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

} // namespace

HRESULT GetContainerAccessible(HWND hwnd, ComPtr<IAccessible>& outAcc) {
    if (!IsWindow(hwnd)) {
        return E_INVALIDARG;
    }
    // SendMessageW correctly dispatches to hwnd's owning thread and, since this always runs
    // before our subclass is installed, reaches the container's real original WM_GETOBJECT
    // handler — see the header comment for why this must not be CallWindowProc.
    LRESULT lresult = SendMessageW(hwnd, WM_GETOBJECT, 0, OBJID_CLIENT);
    if (lresult == 0) {
        return E_FAIL;
    }
    IAccessible* raw = nullptr;
    HRESULT hr = ObjectFromLresult(lresult, IID_IAccessible, 0, reinterpret_cast<void**>(&raw));
    if (FAILED(hr)) {
        return hr;
    }
    outAcc.Attach(raw);
    return S_OK;
}

std::vector<AccessibleRef> GetChildren(IAccessible* parent, const VARIANT& parentChildId) {
    std::vector<AccessibleRef> result;
    if (!parent || parentChildId.vt != VT_I4 || parentChildId.lVal != CHILDID_SELF) {
        // MSAA "simple children" (plain integer childIds) have no children of their own.
        return result;
    }

    long childCount = 0;
    if (FAILED(parent->get_accChildCount(&childCount)) || childCount <= 0) {
        return result;
    }

    std::vector<VARIANT> children(childCount);
    long fetched = 0;
    HRESULT hr = AccessibleChildren(parent, 0, childCount, children.data(), &fetched);
    if (FAILED(hr)) {
        return result;
    }

    for (long i = 0; i < fetched; ++i) {
        AccessibleRef ref;
        if (children[i].vt == VT_DISPATCH && children[i].pdispVal) {
            IAccessible* childAcc = nullptr;
            if (SUCCEEDED(children[i].pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(&childAcc)))) {
                ref.acc.Attach(childAcc);
                ref.childId.vt = VT_I4;
                ref.childId.lVal = CHILDID_SELF;
                result.push_back(ref);
            }
            VariantClear(&children[i]);
        } else if (children[i].vt == VT_I4) {
            ref.acc = ComPtr<IAccessible>(parent);
            parent->AddRef();
            ref.childId.vt = VT_I4;
            ref.childId.lVal = children[i].lVal;
            result.push_back(ref);
        }
    }
    return result;
}

AccessibleNodeInfo GetNodeInfo(IAccessible* acc, const VARIANT& childId) {
    AccessibleNodeInfo info;
    if (!acc) {
        return info;
    }

    VARIANT selfId = childId;

    BSTR name = nullptr;
    if (SUCCEEDED(acc->get_accName(selfId, &name)) && name) {
        info.name.assign(name, SysStringLen(name));
        SysFreeString(name);
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
    IAccessible* root,
    const VARIANT& rootChildId,
    LocateStrategy strategy,
    const std::wstring& value,
    bool multiple) {
    std::vector<AccessibleRef> result;

    // BFS to keep result ordering stable/predictable across calls and to bound recursion depth
    // for pathological trees.
    struct QueueItem { ComPtr<IAccessible> acc; VARIANT childId; };
    std::vector<QueueItem> queue;
    {
        QueueItem item;
        item.acc = ComPtr<IAccessible>(root);
        root->AddRef();
        item.childId = rootChildId;
        queue.push_back(item);
    }

    for (size_t i = 0; i < queue.size(); ++i) {
        AccessibleNodeInfo info = GetNodeInfo(queue[i].acc.Get(), queue[i].childId);
        if (MatchesLocator(info, strategy, value)) {
            AccessibleRef ref;
            ref.acc = queue[i].acc;
            ref.childId = queue[i].childId;
            result.push_back(ref);
            if (!multiple) {
                return result;
            }
        }
        for (auto& child : GetChildren(queue[i].acc.Get(), queue[i].childId)) {
            QueueItem item;
            item.acc = child.acc;
            item.childId = child.childId;
            queue.push_back(item);
        }
    }

    return result;
}

} // namespace UiaBridge
