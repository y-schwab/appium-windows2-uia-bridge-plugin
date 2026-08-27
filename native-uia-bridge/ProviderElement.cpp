#include "ProviderElement.h"
#include "ProviderRoot.h"
#include "UiaMapping.h"

namespace UiaBridge {

ProviderElement::ProviderElement(
    HWND hwnd,
    ProviderRoot* root,
    ElementRegistry* registry,
    const AccessibleRef& self,
    const AccessibleRef& parentRef,
    IRawElementProviderFragment* parentFragment)
    : hwnd_(hwnd)
    , root_(root)
    , registry_(registry)
    , self_(self)
    , parentRef_(parentRef)
    , parentFragment_(parentFragment) {
    if (parentFragment_) {
        parentFragment_->AddRef();
    }
    internalId_ = registry_->Register(self_);
}

ProviderElement::~ProviderElement() {
    if (parentFragment_) {
        parentFragment_->Release();
    }
}

HRESULT STDMETHODCALLTYPE ProviderElement::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) { return E_POINTER; }
    if (riid == IID_IUnknown || riid == __uuidof(IRawElementProviderSimple)) {
        *ppvObject = static_cast<IRawElementProviderSimple*>(this);
    } else if (riid == __uuidof(IRawElementProviderFragment)) {
        *ppvObject = static_cast<IRawElementProviderFragment*>(this);
    } else if (riid == __uuidof(IInvokeProvider)) {
        *ppvObject = static_cast<IInvokeProvider*>(this);
    } else if (riid == __uuidof(IValueProvider)) {
        *ppvObject = static_cast<IValueProvider*>(this);
    } else {
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE ProviderElement::AddRef() {
    return ++refCount_;
}

ULONG STDMETHODCALLTYPE ProviderElement::Release() {
    ULONG count = --refCount_;
    if (count == 0) {
        delete this;
    }
    return count;
}

HRESULT STDMETHODCALLTYPE ProviderElement::get_ProviderOptions(ProviderOptions* pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = ProviderOptions_ServerSideProvider;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderElement::GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = nullptr;
    if (patternId == UIA_InvokePatternId) {
        *pRetVal = static_cast<IInvokeProvider*>(this);
        AddRef();
    } else if (patternId == UIA_ValuePatternId) {
        *pRetVal = static_cast<IValueProvider*>(this);
        AddRef();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderElement::GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    VariantInit(pRetVal);

    AccessibleNodeInfo info = GetNodeInfo(self_);

    switch (propertyId) {
        case UIA_NamePropertyId:
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(info.name.c_str());
            break;
        case UIA_ControlTypePropertyId:
            pRetVal->vt = VT_I4;
            pRetVal->lVal = ControlTypeNameToUiaId(info.controlType);
            break;
        case UIA_AutomationIdPropertyId:
            // Already extracted by GetNodeInfo (real OLE control Name when available, else a
            // synthesized role:name fallback) but never previously surfaced here.
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(info.automationId.c_str());
            break;
        case UIA_ValueValuePropertyId:
            // The Value *pattern*'s get_Value (below) already returns this — but some clients
            // (Inspect's flat property list among them) read the raw property instead of going
            // through the pattern interface, and that path had no case here at all previously.
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(info.value.c_str());
            break;
        case UIA_HelpTextPropertyId:
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(info.helpText.c_str());
            break;
        case UIA_AccessKeyPropertyId:
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(info.accessKey.c_str());
            break;
        case UIA_IsEnabledPropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = info.isEnabled ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_IsInvokePatternAvailablePropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = VARIANT_TRUE;
            break;
        case UIA_IsValuePatternAvailablePropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = VARIANT_TRUE;
            break;
        case UIA_HasKeyboardFocusPropertyId:
        case UIA_IsKeyboardFocusablePropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = VARIANT_FALSE;
            break;
        default:
            break; // Leave VT_EMPTY — UIA core treats that as "not supported".
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderElement::get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    // Only the fragment root is the "host" tie-in point (see ProviderRoot::get_HostRawElementProvider)
    // — individual child elements have no window of their own to host.
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderElement::Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = nullptr;

    switch (direction) {
        case NavigateDirection_Parent:
            if (parentFragment_) {
                parentFragment_->AddRef();
                *pRetVal = parentFragment_;
            }
            return S_OK;

        case NavigateDirection_FirstChild:
        case NavigateDirection_LastChild: {
            auto children = GetChildren(self_);
            if (children.empty()) { return S_OK; }
            const AccessibleRef& child = (direction == NavigateDirection_FirstChild) ? children.front() : children.back();
            *pRetVal = new ProviderElement(hwnd_, root_, registry_, child, self_, this);
            return S_OK;
        }

        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling: {
            if (!parentRef_.acc) { return S_OK; } // root-level element under the container has no MSAA "parent" to re-enumerate
            auto siblings = GetChildren(parentRef_);
            for (size_t i = 0; i < siblings.size(); ++i) {
                bool sameChild = siblings[i].acc.Get() == self_.acc.Get() && siblings[i].childId.lVal == self_.childId.lVal;
                if (!sameChild) { continue; }
                size_t neighborIndex = (direction == NavigateDirection_NextSibling) ? i + 1 : i - 1;
                if (direction == NavigateDirection_PreviousSibling && i == 0) { return S_OK; }
                if (neighborIndex >= siblings.size()) { return S_OK; }
                *pRetVal = new ProviderElement(hwnd_, root_, registry_, siblings[neighborIndex], parentRef_, parentFragment_);
                return S_OK;
            }
            return S_OK;
        }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderElement::GetRuntimeId(SAFEARRAY** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    long uniqueValue = static_cast<long>(std::hash<std::wstring>{}(internalId_));
    SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!sa) { return E_OUTOFMEMORY; }
    LONG index = 0;
    long first = static_cast<long>(UiaAppendRuntimeId);
    SafeArrayPutElement(sa, &index, &first);
    index = 1;
    SafeArrayPutElement(sa, &index, &uniqueValue);
    *pRetVal = sa;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderElement::get_BoundingRectangle(UiaRect* pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    AccessibleNodeInfo info = GetNodeInfo(self_);
    pRetVal->left = info.rectScreen.left;
    pRetVal->top = info.rectScreen.top;
    pRetVal->width = info.rectScreen.right - info.rectScreen.left;
    pRetVal->height = info.rectScreen.bottom - info.rectScreen.top;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderElement::GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderElement::SetFocus() {
    return S_OK; // No-op in v1 — MSAA has no reliable cross-control focus setter beyond accSelect, which is state-specific.
}

HRESULT STDMETHODCALLTYPE ProviderElement::get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    return root_->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(pRetVal));
}

HRESULT STDMETHODCALLTYPE ProviderElement::Invoke() {
    HRESULT hr = InvokeDefaultAction(self_.acc.Get(), self_.childId);
    return SUCCEEDED(hr) ? S_OK : UIA_E_NOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE ProviderElement::SetValue(LPCWSTR val) {
    if (!val) { return E_INVALIDARG; }
    HRESULT hr = SetNodeValue(self_.acc.Get(), self_.childId, val);
    return SUCCEEDED(hr) ? S_OK : UIA_E_NOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE ProviderElement::get_Value(BSTR* pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    AccessibleNodeInfo info = GetNodeInfo(self_);
    *pRetVal = SysAllocString(info.value.c_str());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProviderElement::get_IsReadOnly(BOOL* pRetVal) {
    if (!pRetVal) { return E_POINTER; }
    *pRetVal = FALSE;
    return S_OK;
}

} // namespace UiaBridge
