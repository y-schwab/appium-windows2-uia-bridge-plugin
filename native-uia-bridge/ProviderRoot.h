#pragma once
// UIA fragment-root provider for the Forms 2.0 container itself. Combines
// IRawElementProviderSimple + IRawElementProviderFragment + IRawElementProviderFragmentRoot on
// one object, same shape as Microsoft's UIAutomationFragmentProvider sample — the root needs
// Fragment (to be navigable as a tree node: FirstChild, BoundingRectangle, ...) as well as
// FragmentRoot (ElementProviderFromPoint, GetFocus) since it's both the entry point UIA is
// handed via WM_GETOBJECT *and* the top node of the exposed subtree.

#include <uiautomation.h>
#include <windows.h>
#include <atomic>

#include "AccessibleTree.h"
#include "ElementRegistry.h"

namespace UiaBridge {

class ProviderRoot
    : public IRawElementProviderSimple
    , public IRawElementProviderFragment
    , public IRawElementProviderFragmentRoot {
public:
    ProviderRoot(HWND hwnd, const AccessibleRef& rootRef);
    virtual ~ProviderRoot();

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

    // IRawElementProviderFragmentRoot
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** pRetVal) override;

    ElementRegistry& Registry() { return registry_; }
    HWND Hwnd() const { return hwnd_; }
    const AccessibleRef& RootRef() const { return rootRef_; }

private:
    std::atomic<ULONG> refCount_{1};
    HWND hwnd_;
    AccessibleRef rootRef_;
    ElementRegistry registry_;
};

} // namespace UiaBridge
