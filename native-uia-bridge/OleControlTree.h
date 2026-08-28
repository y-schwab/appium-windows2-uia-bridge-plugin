#pragma once
// Tier 2 (see NEXT_STEPS.md's extraction-tier table): Forms 2.0 / VBA-style windowless-control
// introspection via the OLE embedding model (WM_GETOBJECT/OBJID_NATIVEOM -> IDispatch), bypassing
// MSAA entirely. This was ripped out once when it came back empty for one specific target app —
// wrongly: that was one app's accessibility plumbing being broken, not evidence this whole
// approach is dead. Re-added as a live, best-effort fallback tier for whichever future app *does*
// answer OBJID_NATIVEOM, tried after MSAA/window-text (tiers 0-1) and before GDI paint capture
// (tiers 3-4) — see GetNodeInfo in AccessibleTree.cpp for where it's consulted.
//
// Deliberately narrower than the original version of this file: only fetches property values for
// a node whose hwnd is already known (via MSAA/window-enum discovery, tiers 0-1) — it no longer
// does its own tree enumeration (IOleContainer::EnumObjects / Controls-collection walking), since
// child discovery is handled uniformly by AccessibleTree.cpp's existing MSAA+hwnd-walk regardless
// of which tier ultimately supplies a given node's name/value.

#include <string>
#include <windows.h>

#include "ComPtr.h"

namespace UiaBridge {

// Sends WM_GETOBJECT(OBJID_NATIVEOM) to `hwnd` — the documented mechanism VB/VBA forms use to
// expose their native object model (the UserForm's own IDispatch, with a `Controls` collection)
// to automation tools. Safe to call at any time, including after our subclass is installed:
// SubclassProc (WindowSubclass.cpp) only intercepts OBJID_CLIENT/UiaRootObjectId, forwarding
// everything else — OBJID_NATIVEOM included — straight to the original wndproc, so this can't
// recurse into our own provider.
HRESULT GetRootOleDispatch(HWND hwnd, ComPtr<IDispatch>& outDispatch);

struct OleControlInfo {
    std::wstring name;         // control's real VBA-assigned Name property
    std::wstring controlType;  // ProgID via IPersist::GetClassID, e.g. "Forms.CommandButton.1" — empty if unavailable
    std::wstring text;         // Caption, else Text, else Value — whichever the control actually has
    std::wstring helpText;     // ControlTipText — the control's own designer-set tooltip, if any
    std::wstring accessKey;    // Accelerator — the single-character mnemonic key, if any
    bool enabled = true;
    bool visible = true;
};

// Property reads via IDispatch::GetIDsOfNames + Invoke(DISPATCH_PROPERTYGET). Missing/unsupported
// properties are left at their OleControlInfo default rather than erroring — property availability
// varies by control type (CommandButton has no Text, TextBox has no Caption, etc). Each property
// read is individually guarded against a misbehaving third-party COM object raising a structured
// exception out of Invoke (see the .cpp) — a broken automation object degrades this one tier to
// "no answer," not a crash of the host process.
OleControlInfo GetOleControlInfo(IDispatch* dispatch);

} // namespace UiaBridge
