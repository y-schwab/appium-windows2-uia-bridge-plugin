#include "OleControlTree.h"
#include "Diagnostics.h"

#include <objbase.h>
#include <oleacc.h> // ObjectFromLresult

#pragma comment(lib, "ole32.lib") // ProgIDFromCLSID, CoTaskMemFree
#pragma comment(lib, "oleacc.lib") // ObjectFromLresult

namespace UiaBridge {

namespace {

// Every call into `dispatch` here runs arbitrary third-party code — whatever COM object the
// target app's automation model happens to be, which for a "best effort, never break execution"
// fallback tier has to be assumed capable of misbehaving (raising a structured exception/AV
// instead of returning a failing HRESULT, which some legacy/third-party automation
// implementations are known to do on edge cases). Both functions here are deliberately kept free
// of C++ objects with destructors (only POD structs: DISPPARAMS, VARIANT, EXCEPINFO) — that's
// what makes __try/__except valid in them under this project's /EHsc; a function mixing __try
// with C++ objects needing unwinding is a compile error without /EHa. A crash here degrades this
// one property read to "unavailable" and falls through to the next tier, not a crash of the host
// process this DLL is injected into.
bool GetIdOfName(IDispatch* dispatch, const wchar_t* name, DISPID* outDispid) {
    LPOLESTR nameCopy = const_cast<LPOLESTR>(name);
    HRESULT hr = E_FAIL;
    __try {
        hr = dispatch->GetIDsOfNames(IID_NULL, &nameCopy, 1, LOCALE_USER_DEFAULT, outDispid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = E_UNEXPECTED;
    }
    return SUCCEEDED(hr);
}

HRESULT SehGuardedInvoke(IDispatch* dispatch, DISPID dispid, WORD flags, DISPPARAMS* params, VARIANT* result) {
    EXCEPINFO excepInfo{};
    HRESULT hr = E_FAIL;
    __try {
        hr = dispatch->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, params, result, &excepInfo, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = E_UNEXPECTED;
    }
    return hr;
}

bool GetDispatchProperty(IDispatch* dispatch, const wchar_t* name, VARIANT* outValue) {
    VariantInit(outValue);
    if (!dispatch) { return false; }

    DISPID dispid = DISPID_UNKNOWN;
    if (!GetIdOfName(dispatch, name, &dispid)) { return false; }

    DISPPARAMS params{};
    return SUCCEEDED(SehGuardedInvoke(dispatch, dispid, DISPATCH_PROPERTYGET, &params, outValue));
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

} // namespace

HRESULT GetRootOleDispatch(HWND hwnd, ComPtr<IDispatch>& outDispatch) {
    if (!IsWindow(hwnd)) {
        return E_INVALIDARG;
    }

    LRESULT lresult = SendMessageW(hwnd, WM_GETOBJECT, 0, OBJID_NATIVEOM);
    if (lresult == 0) {
        return E_FAIL;
    }

    IDispatch* raw = nullptr;
    HRESULT hr = ObjectFromLresult(lresult, IID_IDispatch, 0, reinterpret_cast<void**>(&raw));
    if (FAILED(hr) || !raw) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    outDispatch.Attach(raw);
    return S_OK;
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
    GetStringProperty(dispatch, L"ControlTipText", &info.helpText);
    GetStringProperty(dispatch, L"Accelerator", &info.accessKey);

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
