#include "ProviderRoot.h"
#include "ProviderElement.h"
#include "UiaMapping.h"

#pragma comment(lib, "UIAutomationCore.lib") // UiaHostProviderFromHwnd
#pragma comment(lib, "oleaut32.lib") // SafeArray*/Variant*/BSTR helpers used throughout this file and ProviderElement.cpp

namespace UiaBridge {

ProviderRoot::ProviderRoot(HWND hwnd, const AccessibleRef& rootRef)
    : hwnd_(hwnd)
    , rootRef_(rootRef) {
    registry_.RegisterRoot(rootRef_);
}

ProviderRoot::~ProviderRoot() {}

HRESULT STDMETHODCALLTYPE ProviderRoot::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) { return E_POINTER; }
    if (riid == IID_IUnknown || riid == __uuidof(IRawElementProviderSimple)) {
        *ppvObject = static_cast<IRawElementProviderSimple*>(this);
    } else if (riid == __uuidof(IRawElementProviderFragment)) {
        *ppvObject = static_cast<IRawElementProviderFragment*>(this);
    } else if (riid == __uuidof(IRawElementProviderFragmentRoot)) {
        *ppvObject = static_cast<IRawElementProviderFragmentRoot*>(this);
    } else {
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE ProviderRoot::AddRef() {
    return ++refCount_;
}

ULONG STDMETHODCALLTYPE ProviderRoot::Release() {
    ULONG count = --refCount_;
    if (count == 0) {
        delete this;
    }
    return count;
}

HRESULT STDMETHODCALLTYPE ProviderRoot::get_ProviderOptions(ProviderOptions* pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = ProviderOptions_ServerSideProvider;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderRoot::GetPatternProvider(PATTERNID /*patternId*/, IUnknown** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = nullptr; // The container itself exposes no patterns — only its children do.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderRoot::GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    VariantInit(pRetVal);
    AccessibleNodeInfo info = GetNodeInfo(rootRef_);

    switch (propertyId) {
        case UIA_NamePropertyId:
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(info.name.c_str());
            break;
        case UIA_ControlTypePropertyId:
            pRetVal->vt = VT_I4;
            pRetVal->lVal = UIA_PaneControlTypeId; // The container is a generic content pane.
            break;
        case UIA_IsEnabledPropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = info.isEnabled ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        default:
            break;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderRoot::get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    // Ties this fragment root to the real hwnd's own automation provider, so UIA core handles
    // ancestor navigation (up to the real parent window, desktop, etc.) automatically instead of
    // us reimplementing it — see Navigate()'s NavigateDirection_Parent below.
    return UiaHostProviderFromHwnd(hwnd_, pRetVal);
}

HRESULT STDMETHODCALLTYPE ProviderRoot::Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = nullptr;

    switch (direction) {
        case NavigateDirection_Parent:
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling:
            return S_OK; // Handled by the host provider chain (see get_HostRawElementProvider).

        case NavigateDirection_FirstChild:
        case NavigateDirection_LastChild: {
            auto children = GetChildren(rootRef_);
            if (children.empty()) { return S_OK; }
            const AccessibleRef& child = (direction == NavigateDirection_FirstChild) ? children.front() : children.back();
            *pRetVal = new ProviderElement(hwnd_, this, &registry_, child, rootRef_, this);
            return S_OK;
        }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderRoot::GetRuntimeId(SAFEARRAY** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = nullptr; // Per UIA docs: fragment roots return NULL — the runtime id is derived from the hwnd.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderRoot::get_BoundingRectangle(UiaRect* pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    AccessibleNodeInfo info = GetNodeInfo(rootRef_);
    pRetVal->left = info.rectScreen.left;
    pRetVal->top = info.rectScreen.top;
    pRetVal->width = info.rectScreen.right - info.rectScreen.left;
    pRetVal->height = info.rectScreen.bottom - info.rectScreen.top;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderRoot::GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderRoot::SetFocus() {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderRoot::get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    AddRef();
    *pRetVal = this;
    return S_OK;
}

namespace {

bool RectContains(const RECT& r, double x, double y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Depth-first hit test — returns the deepest (most specific) matching descendant, matching how
// ElementProviderFromPoint is expected to behave (innermost element under the cursor).
bool HitTestRecursive(HWND hwnd, ProviderRoot* root, ElementRegistry* registry,
                       const AccessibleRef& node,
                       IRawElementProviderFragment* parentFragment,
                       double x, double y, ProviderElement** outMatch) {
    for (auto& child : GetChildren(node)) {
        AccessibleNodeInfo info = GetNodeInfo(child);
        if (!RectContains(info.rectScreen, x, y)) { continue; }
        auto* childElement = new ProviderElement(hwnd, root, registry, child, node, parentFragment);
        ProviderElement* deeper = nullptr;
        if (HitTestRecursive(hwnd, root, registry, child, childElement, x, y, &deeper)) {
            childElement->Release();
            *outMatch = deeper;
            return true;
        }
        *outMatch = childElement;
        return true;
    }
    return false;
}

} // namespace

HRESULT STDMETHODCALLTYPE ProviderRoot::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = nullptr;

    ProviderElement* match = nullptr;
    if (HitTestRecursive(hwnd_, this, &registry_, rootRef_, this, x, y, &match)) {
        *pRetVal = match;
        return S_OK;
    }
    return S_OK; // No child under the point — UIA falls back to this root itself.
}

HRESULT STDMETHODCALLTYPE ProviderRoot::GetFocus(IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = nullptr; // TODO(v2): derive from accFocus — not required for the invoke/find scope of v1.
    return S_OK;
}

} // namespace UiaBridge
