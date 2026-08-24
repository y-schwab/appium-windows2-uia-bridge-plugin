#pragma once
// UIA fragment provider for one child control inside the Forms 2.0 container. Implements
// IRawElementProviderSimple + IRawElementProviderFragment (tree navigation) + IInvokeProvider +
// IValueProvider (mapped onto the underlying IAccessible's accDoDefaultAction/put_accValue) —
// the interfaces the task spec calls out.

#include <uiautomation.h>
#include <windows.h>
#include <atomic>

#include "AccessibleTree.h"
#include "ElementRegistry.h"

namespace UiaBridge {

class ProviderRoot;

class ProviderElement
    : public IRawElementProviderSimple
    , public IRawElementProviderFragment
    , public IInvokeProvider
    , public IValueProvider {
public:
    // `parentFragment` is AddRef'd by the caller and owned by this instance (released in the
    // destructor) — used to answer NavigateDirection_Parent and to re-derive siblings via
    // `parentRef`. `parentRef` (not just its IAccessible) is needed, not only its ComPtr, because
    // GetChildren() needs the parent's hwnd too to correctly re-walk sibling child windows.
    // `root` is a non-owning back-pointer: ProviderRoot outlives every ProviderElement created
    // under it (all released together on detach, see WindowSubclass.cpp).
    ProviderElement(
        HWND hwnd,
        ProviderRoot* root,
        ElementRegistry* registry,
        const AccessibleRef& self,
        const AccessibleRef& parentRef,
        IRawElementProviderFragment* parentFragment);
    virtual ~ProviderElement();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override;

    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) override;

    // IInvokeProvider
    HRESULT STDMETHODCALLTYPE Invoke() override;

    // IValueProvider
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR val) override;
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* pRetVal) override;

    const std::wstring& InternalId() const { return internalId_; }
    const AccessibleRef& Self() const { return self_; }

private:
    std::atomic<ULONG> refCount_{1};
    HWND hwnd_;
    ProviderRoot* root_; // non-owning
    ElementRegistry* registry_; // non-owning
    AccessibleRef self_;
    AccessibleRef parentRef_;
    IRawElementProviderFragment* parentFragment_; // owned, AddRef'd in ctor
    std::wstring internalId_;
};

} // namespace UiaBridge
