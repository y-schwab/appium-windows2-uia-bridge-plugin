#pragma once
// Forms 2.0 windowless-control discovery via the OLE embedding model (IOleContainer /
// IDispatch), bypassing MSAA entirely. See AccessibleTree.h's oleControl field on AccessibleRef —
// this module is only ever invoked from there, once a node's oleControl pointer is set.
//
// Root acquisition (GetRootOleDispatch) is the one hwnd-specific step: everything past that point
// (EnumOleChildrenOfDispatch, GetOleControlInfo) operates uniformly on any IDispatch, whether it's
// the root UserForm's own native-object-model dispatch or a nested container control's (Frame,
// MultiPage page) — Forms 2.0 controls that are themselves containers expose the same IOleContainer
// / Controls-collection shape as the root, so the same walk recurses into them for free.

#include <ocidl.h>
#include <oleauto.h>
#include <string>
#include <vector>
#include <windows.h>

#include "ComPtr.h"

namespace UiaBridge {

// Sends WM_GETOBJECT(OBJID_NATIVEOM) to `hwnd` — the documented mechanism VB/VBA forms use to
// expose their native object model (the UserForm's own IDispatch, with a `Controls` collection)
// to automation tools. Must be called before our subclass is installed, same as
// GetContainerAccessible in AccessibleTree.cpp/.h — SendMessageW needs to reach the container's
// real original WM_GETOBJECT handler, not our own SubclassProc.
HRESULT GetRootOleDispatch(HWND hwnd, ComPtr<IDispatch>& outDispatch);

// Enumerates `dispatch`'s embedded child controls. Tries QueryInterface for IOleContainer first
// (IOleContainer::EnumObjects(OLECONTF_EMBEDDINGS, ...)) — the mechanism the task spec named.
// Falls back to invoking `dispatch`'s own `Controls` property and walking the returned collection
// via its _NewEnum/IEnumVARIANT, for objects (in practice: every Forms 2.0 control and the
// UserForm itself) that expose child controls that way instead of/in addition to IOleContainer.
std::vector<ComPtr<IDispatch>> EnumOleChildrenOfDispatch(IDispatch* dispatch);

struct OleControlInfo {
    std::wstring name;         // control's real VBA-assigned Name property
    std::wstring controlType;  // ProgID via IPersist::GetClassID, e.g. "Forms.CommandButton.1" — empty if unavailable
    std::wstring text;         // Caption, else Text, else Value — whichever the control actually has
    std::wstring helpText;     // ControlTipText — the control's own designer-set tooltip, if any
    std::wstring accessKey;    // Accelerator — the single-character mnemonic key, if any
    bool enabled = true;
    bool visible = true;
    RECT rectPointsLocal{};    // Left/Top/Width/Height in points, relative to the immediate parent's client origin
};

// Property reads via IDispatch::GetIDsOfNames + Invoke(DISPATCH_PROPERTYGET). Missing/unsupported
// properties are left at their OleControlInfo default rather than erroring — property availability
// varies by control type (CommandButton has no Text, TextBox has no Caption, etc).
OleControlInfo GetOleControlInfo(IDispatch* dispatch);

} // namespace UiaBridge
