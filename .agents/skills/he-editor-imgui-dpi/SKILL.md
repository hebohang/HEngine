---
name: he-editor-imgui-dpi
description: Apply the repository's ImGui DPI-scaling conventions to layout, size, spacing, or column changes under Engine/Source/Editor.
---

# Editor DPI rules

Use `Editor/ImGuiWrapper/ImGuiWrapper.h` helpers: `S(...)`, `SetColumnWidth`, `SetNextWindowSize`, `SetNextWindowSizeConstraints`, and `SameLine`.

- Do not add local `* uiScale` calculations in Editor code.
- Refresh cached column widths when `HasScaleChanged(...)` fires.
- Do not rescale values already in final pixel space, such as viewport/work-area coordinates.

Validate the affected panel through the suitable Editor route in `he-build-validate`;
reuse its build result instead of adding a separate build.
