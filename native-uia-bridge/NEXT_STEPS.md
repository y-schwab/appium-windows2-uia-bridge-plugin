# Discovery paths — where we are, what's confirmed, what's next

## Confirmed (2026-08-26 real-device diagnostic run)

- `FM20.DLL` genuinely LOADED at base `0x60000000` in the target process. This is
  real Microsoft Forms 2.0, not a squatted classname — `"F3 Server 60000000"` is
  FM20's own documented windowed-control class naming (`"F3 Server " + hex(hInstance
  of FM20.DLL)`), confirmed by the base address matching exactly.
- Despite that, `WM_GETOBJECT` is unanswered (lresult=0) at **every** level: the root
  `#32770` dialog itself (`OBJID_CLIENT` — MSAA falls back to a generic oleacc proxy,
  not the real object) and all 6 `F3 Server 60000000` children (`OBJID_NATIVEOM`,
  `hr=0x80004005`/E_FAIL from `ObjectFromLresult` on lresult 0).
- Conclusion: this is not "wrong DLL" or "wrong theory" — it's the *host app*
  (`MSV.EXE`, BCCLTD) not forwarding `WM_GETOBJECT` down to FM20's own handling
  (custom subclassing swallowing it, and/or controls not fully in-place-activated as
  real OLE objects by their container). Both COM-based discovery paths (MSAA, native
  OM) are dead ends on solid evidence now, not a guess — no third COM avenue is worth
  trying here.

## Path 1 — GDI text-draw hooking (IMPLEMENTING NOW)

IAT-patch `TextOutW/A`, `ExtTextOutW/A`, `DrawTextW/A`, `DrawTextExW/A` inside
FM20.DLL's own import table, capture whatever string gets painted tagged by the hwnd
the DC belongs to (`WindowFromDC`). Doesn't care what object model backs the control —
only what it paints. Strongest remaining path since accessibility plumbing is
confirmed broken at the host-app level and isn't fixable from outside.

## Path 2 — Direct OLE site reflection, bypassing the broken container (fallback, low confidence)

Try to recover FM20's `IOleClientSite`/site pointer directly (e.g. window props FM20
stashes on the hwnd) instead of going through `IOleContainer::EnumObjects` on the
parent. Since root MSAA already fails too, likely hits the same dead end — only worth
trying if Path 1 turns out insufficient (e.g. can't attribute painted text to the
right control reliably).

## Path 3 — Geometry + paint-capture only, no live name/value introspection (fallback)

Give up on any accessibility-API introspection entirely. Rely solely on GDI-paint
capture (Path 1) for values/text, paired with hwnd rects (already working) for
element bounds and click/type-by-rect interaction. This is where we land if Path 1's
capture turns out unreliable/inconsistent but partial capture is still better than
nothing.

## Removed this session (dead ends, ripped out to keep the codebase honest)

- `OleControlTree.h/.cpp` and all `AccessibleRef::oleControl` plumbing in
  `AccessibleTree.h/.cpp`, `WindowSubclass.cpp` — IOleContainer/native-OM discovery
  path, confirmed dead per above.
- `LogLoadedModules()` (`Diagnostics.h/.cpp`) and its call in `DllMain.cpp` — one-shot
  diagnostic that already answered its question (FM20.DLL confirmed loaded); no
  longer needed clogging every attach log.
