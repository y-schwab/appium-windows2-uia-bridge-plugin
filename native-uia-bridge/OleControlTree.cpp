#include "OleControlTree.h"
#include "Diagnostics.h"

#include <objbase.h>
#include <oleacc.h> // ObjectFromLresult

#pragma comment(lib, "ole32.lib") // ProgIDFromCLSID, CoTaskMemFree
#pragma comment(lib, "oleacc.lib") // ObjectFromLresult

namespace UiaBridge {

namespace {

// -4 is DISPID_NEWENUM's fixed value per the Automation spec — used directly rather than relying
// on the macro being defined by every SDK header combination.
constexpr DISPID kDispidNewEnum = -4;

bool GetDispatchProperty(IDispatch* dispatch, const wchar_t* name, VARIANT* outValue) {
    VariantInit(outValue);
    if (!dispatch) { return false; }

    LPOLESTR nameCopy = const_cast<LPOLESTR>(name);
    DISPID dispid = DISPID_UNKNOWN;
    if (FAILED(dispatch->GetIDsOfNames(IID_NULL, &nameCopy, 1, LOCALE_USER_DEFAULT, &dispid))) {
        return false;
    }

    DISPPARAMS params{};
    EXCEPINFO excepInfo{};
    HRESULT hr = dispatch->Invoke(
        dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &params, outValue, &excepInfo, nullptr);
    return SUCCEEDED(hr);
}

bool GetStringProperty(IDispatch* dispatch, const wchar_t* name, std::wstring* outValue) {
    VARIANT value;
    if (!GetDispatchProperty(dispatch, name, &value)) { return false; }
    bool ok = false;
    if (value.vt == VT_BSTR && value.bstrVal) {
        outValue->assign(value.bstrVal, SysStringLen(value.bstrVal));
        ok = true;
    } else if (SUCCEEDED(VariantChangeType(&value, &value, 0, VT_BSTR)) && value.bstrVal) {
        outValue->assign(value.bstrVal, SysStringLen(value.bstrVal));
        ok = true;
    }
    VariantClear(&value);
    return ok;
}

bool GetBoolProperty(IDispatch* dispatch, const wchar_t* name, bool* outValue) {
    VARIANT value;
    if (!GetDispatchProperty(dispatch, name, &value)) { return false; }
    bool ok = false;
    if (SUCCEEDED(VariantChangeType(&value, &value, 0, VT_BOOL))) {
        *outValue = value.boolVal != VARIANT_FALSE;
        ok = true;
    }
    VariantClear(&value);
    return ok;
}

bool GetLongProperty(IDispatch* dispatch, const wchar_t* name, long* outValue) {
    VARIANT value;
    if (!GetDispatchProperty(dispatch, name, &value)) { return false; }
    bool ok = false;
    if (SUCCEEDED(VariantChangeType(&value, &value, 0, VT_I4))) {
        *outValue = value.lVal;
        ok = true;
    }
    VariantClear(&value);
    return ok;
}

// Enumerates `dispatch`'s `Controls` collection property via the standard COM Automation
// collection protocol: fetch `Controls`, invoke its DISPID_NEWENUM member to get an IEnumVARIANT,
// walk it one item at a time. This is how VBA's own `For Each ctrl In Controls` works under the
// hood — every Forms 2.0 container (the UserForm itself, Frame, MultiPage page) exposes its
// children this way, independent of whether it also implements IOleContainer.
std::vector<ComPtr<IDispatch>> EnumViaControlsProperty(IDispatch* dispatch) {
    std::vector<ComPtr<IDispatch>> result;

    VARIANT controlsVar;
    if (!GetDispatchProperty(dispatch, L"Controls", &controlsVar) || controlsVar.vt != VT_DISPATCH || !controlsVar.pdispVal) {
        VariantClear(&controlsVar);
        return result;
    }
    ComPtr<IDispatch> controls;
    controls.Attach(controlsVar.pdispVal); // VariantClear would Release it; take ownership directly instead

    DISPPARAMS params{};
    EXCEPINFO excepInfo{};
    VARIANT enumVar;
    VariantInit(&enumVar);
    HRESULT hr = controls->Invoke(
        kDispidNewEnum, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD | DISPATCH_PROPERTYGET,
        &params, &enumVar, &excepInfo, nullptr);
    if (FAILED(hr) || enumVar.vt != VT_UNKNOWN || !enumVar.punkVal) {
        VariantClear(&enumVar);
        return result;
    }

    ComPtr<IEnumVARIANT> enumVariant;
    IEnumVARIANT* rawEnum = nullptr;
    hr = enumVar.punkVal->QueryInterface(IID_IEnumVARIANT, reinterpret_cast<void**>(&rawEnum));
    VariantClear(&enumVar); // releases punkVal — rawEnum (if obtained) already holds its own AddRef from QI
    if (FAILED(hr) || !rawEnum) {
        return result;
    }
    enumVariant.Attach(rawEnum);

    VARIANT item;
    ULONG fetched = 0;
    while (true) {
        VariantInit(&item);
        if (enumVariant->Next(1, &item, &fetched) != S_OK || fetched == 0) {
            VariantClear(&item);
            break;
        }
        if (item.vt == VT_DISPATCH && item.pdispVal) {
            ComPtr<IDispatch> child;
            child.Attach(item.pdispVal); // take ownership directly, skip VariantClear's Release
            result.push_back(child);
        } else {
            VariantClear(&item);
        }
    }

    return result;
}

} // namespace

HRESULT GetRootOleDispatch(HWND hwnd, ComPtr<IDispatch>& outDispatch) {
    if (!IsWindow(hwnd)) {
        return E_INVALIDARG;
    }

    LRESULT lresult = SendMessageW(hwnd, WM_GETOBJECT, 0, OBJID_NATIVEOM);
    DiagLog(L"GetRootOleDispatch(0x%p): SendMessageW(WM_GETOBJECT, OBJID_NATIVEOM) -> lresult=%lld", hwnd, static_cast<long long>(lresult));
    if (lresult == 0) {
        return E_FAIL;
    }

    IDispatch* raw = nullptr;
    HRESULT hr = ObjectFromLresult(lresult, IID_IDispatch, 0, reinterpret_cast<void**>(&raw));
    if (FAILED(hr) || !raw) {
        DiagLog(L"GetRootOleDispatch(0x%p): ObjectFromLresult failed (hr=0x%08lX)", hwnd, static_cast<unsigned long>(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }

    outDispatch.Attach(raw);
    return S_OK;
}

std::vector<ComPtr<IDispatch>> EnumOleChildrenOfDispatch(IDispatch* dispatch) {
    std::vector<ComPtr<IDispatch>> result;
    if (!dispatch) { return result; }

    ComPtr<IOleContainer> container;
    IOleContainer* rawContainer = nullptr;
    if (SUCCEEDED(dispatch->QueryInterface(IID_IOleContainer, reinterpret_cast<void**>(&rawContainer))) && rawContainer) {
        container.Attach(rawContainer);

        ComPtr<IEnumUnknown> enumUnknown;
        IEnumUnknown* rawEnum = nullptr;
        if (SUCCEEDED(container->EnumObjects(OLECONTF_EMBEDDINGS, &rawEnum)) && rawEnum) {
            enumUnknown.Attach(rawEnum);
            IUnknown* item = nullptr;
            ULONG fetched = 0;
            while (enumUnknown->Next(1, &item, &fetched) == S_OK && fetched > 0) {
                IDispatch* childDispatch = nullptr;
                if (SUCCEEDED(item->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&childDispatch))) && childDispatch) {
                    ComPtr<IDispatch> child;
                    child.Attach(childDispatch);
                    result.push_back(child);
                }
                item->Release();
                item = nullptr;
            }
            if (!result.empty()) {
                return result;
            }
            // IOleContainer present but reported no embeddings — some Forms controls implement the
            // interface without populating it. Fall through to the Controls-property path below.
        }
    }

    return EnumViaControlsProperty(dispatch);
}

OleControlInfo GetOleControlInfo(IDispatch* dispatch) {
    OleControlInfo info;
    if (!dispatch) { return info; }

    GetStringProperty(dispatch, L"Name", &info.name);

    // Display text: whichever of these the control actually implements — CommandButton/Label use
    // Caption, TextBox uses Text (and/or Value), CheckBox/OptionButton use Value.
    if (!GetStringProperty(dispatch, L"Caption", &info.text)) {
        if (!GetStringProperty(dispatch, L"Text", &info.text)) {
            GetStringProperty(dispatch, L"Value", &info.text);
        }
    }

    GetBoolProperty(dispatch, L"Enabled", &info.enabled);
    GetBoolProperty(dispatch, L"Visible", &info.visible);

    long left = 0, top = 0, width = 0, height = 0;
    GetLongProperty(dispatch, L"Left", &left);
    GetLongProperty(dispatch, L"Top", &top);
    GetLongProperty(dispatch, L"Width", &width);
    GetLongProperty(dispatch, L"Height", &height);
    info.rectPointsLocal = { left, top, left + width, top + height };

    ComPtr<IPersist> persist;
    IPersist* rawPersist = nullptr;
    if (SUCCEEDED(dispatch->QueryInterface(IID_IPersist, reinterpret_cast<void**>(&rawPersist))) && rawPersist) {
        persist.Attach(rawPersist);
        CLSID clsid;
        if (SUCCEEDED(persist->GetClassID(&clsid))) {
            LPOLESTR progId = nullptr;
            if (SUCCEEDED(ProgIDFromCLSID(clsid, &progId)) && progId) {
                info.controlType = progId;
                CoTaskMemFree(progId);
            }
        }
    }

    return info;
}

} // namespace UiaBridge
