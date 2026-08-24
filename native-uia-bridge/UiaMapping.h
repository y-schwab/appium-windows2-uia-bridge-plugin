#pragma once
// Maps AccessibleTree's approximate control-type strings (derived from MSAA roles) to real UIA
// CONTROLTYPEID values, for GetPropertyValue(UIA_ControlTypePropertyId).

#include <string>
#include <uiautomation.h>

namespace UiaBridge {

inline long ControlTypeNameToUiaId(const std::wstring& name) {
    if (name == L"Button") { return UIA_ButtonControlTypeId; }
    if (name == L"Edit") { return UIA_EditControlTypeId; }
    if (name == L"Text") { return UIA_TextControlTypeId; }
    if (name == L"CheckBox") { return UIA_CheckBoxControlTypeId; }
    if (name == L"RadioButton") { return UIA_RadioButtonControlTypeId; }
    if (name == L"ComboBox") { return UIA_ComboBoxControlTypeId; }
    if (name == L"List") { return UIA_ListControlTypeId; }
    if (name == L"ListItem") { return UIA_ListItemControlTypeId; }
    if (name == L"Group") { return UIA_GroupControlTypeId; }
    if (name == L"Pane") { return UIA_PaneControlTypeId; }
    if (name == L"Window") { return UIA_WindowControlTypeId; }
    if (name == L"MenuItem") { return UIA_MenuItemControlTypeId; }
    if (name == L"ScrollBar") { return UIA_ScrollBarControlTypeId; }
    return UIA_CustomControlTypeId;
}

} // namespace UiaBridge
