from __future__ import annotations
import HEngine.UI
import typing

__all__ = [
    "Begin",
    "BeginChild",
    "BeginChildScaled",
    "BeginCombo",
    "BeginGroup",
    "BulletText",
    "Button",
    "ButtonScaled",
    "Checkbox",
    "ClearTextureCache",
    "Col_Button",
    "Col_ButtonActive",
    "Col_ButtonHovered",
    "Col_ChildBg",
    "Col_FrameBg",
    "Col_FrameBgActive",
    "Col_FrameBgHovered",
    "Col_Header",
    "Col_HeaderActive",
    "Col_HeaderHovered",
    "Col_PlotHistogram",
    "Col_PopupBg",
    "Col_Text",
    "Col_TitleBg",
    "Col_TitleBgActive",
    "Col_WindowBg",
    "CollapsingHeader",
    "Columns",
    "DragFloat",
    "DragFloatScaled",
    "DragInt",
    "DragIntScaled",
    "End",
    "EndChild",
    "EndCombo",
    "EndGroup",
    "GetContentRegionAvail",
    "GetCursorPos",
    "GetUIScale",
    "GetViewportOffset",
    "GetViewportSize",
    "GetWindowSize",
    "Image",
    "ImageScaled",
    "Indent",
    "IndentScaled",
    "InputFloat",
    "InputInt",
    "InputText",
    "LabelText",
    "LoadTexture",
    "NewLine",
    "NextColumn",
    "PopStyleColor",
    "PopStyleVar",
    "ProgressBar",
    "ProgressBarScaled",
    "PushStyleColor",
    "PushStyleVar",
    "PushStyleVar2",
    "SameLine",
    "SameLineScaled",
    "Scale",
    "Selectable",
    "Separator",
    "SetColumnWidth",
    "SetColumnWidthScaled",
    "SetCursorPos",
    "SetCursorPosScaled",
    "SetNextWindowBgAlpha",
    "SetNextWindowPos",
    "SetNextWindowPosScaled",
    "SetNextWindowSize",
    "SetNextWindowSizeConstraints",
    "SetNextWindowSizeConstraintsScaled",
    "SetNextWindowSizeScaled",
    "SliderFloat",
    "SliderInt",
    "SmallButton",
    "Spacing",
    "SpacingScaled",
    "StyleVar_Alpha",
    "StyleVar_FramePadding",
    "StyleVar_FrameRounding",
    "StyleVar_IndentSpacing",
    "StyleVar_ItemInnerSpacing",
    "StyleVar_ItemSpacing",
    "StyleVar_WindowBorderSize",
    "StyleVar_WindowPadding",
    "StyleVar_WindowRounding",
    "Text",
    "TextColored",
    "TreeNode",
    "TreePop",
    "Unindent",
    "UnindentScaled",
    "WindowFlags_AlwaysAutoResize",
    "WindowFlags_NoBackground",
    "WindowFlags_NoBringToFrontOnFocus",
    "WindowFlags_NoCollapse",
    "WindowFlags_NoDecoration",
    "WindowFlags_NoInputs",
    "WindowFlags_NoMove",
    "WindowFlags_NoResize",
    "WindowFlags_NoSavedSettings",
    "WindowFlags_NoScrollWithMouse",
    "WindowFlags_NoScrollbar",
    "WindowFlags_NoTitleBar",
    "WindowFlags_None"
]


def Begin(name: str, flags: int = 0) -> bool:
    pass
def BeginChild(id: str, w: float = 0.0, h: float = 0.0, border: bool = False) -> bool:
    pass
def BeginChildScaled(id: str, w: float = 0.0, h: float = 0.0, border: bool = False) -> bool:
    pass
def BeginCombo(arg0: str, arg1: str) -> bool:
    pass
def BeginGroup() -> None:
    pass
def BulletText(arg0: str) -> None:
    pass
def Button(label: str, w: float = 0.0, h: float = 0.0) -> bool:
    pass
def ButtonScaled(label: str, w: float = 0.0, h: float = 0.0) -> bool:
    pass
def Checkbox(arg0: str, arg1: bool) -> typing.Tuple[bool, bool]:
    pass
def ClearTextureCache() -> None:
    pass
def CollapsingHeader(arg0: str) -> bool:
    pass
def Columns(count: int = 1, border: bool = True) -> None:
    pass
def DragFloat(label: str, v: float, speed: float = 1.0, vMin: float = 0.0, vMax: float = 0.0) -> typing.Tuple[bool, float]:
    pass
def DragFloatScaled(label: str, v: float, speed: float = 1.0, vMin: float = 0.0, vMax: float = 0.0) -> typing.Tuple[bool, float]:
    pass
def DragInt(label: str, v: int, speed: float = 1.0, vMin: int = 0, vMax: int = 0) -> typing.Tuple[bool, int]:
    pass
def DragIntScaled(label: str, v: int, speed: float = 1.0, vMin: int = 0, vMax: int = 0) -> typing.Tuple[bool, int]:
    pass
def End() -> None:
    pass
def EndChild() -> None:
    pass
def EndCombo() -> None:
    pass
def EndGroup() -> None:
    pass
def GetContentRegionAvail() -> typing.Tuple[float, float]:
    pass
def GetCursorPos() -> typing.Tuple[float, float]:
    pass
def GetUIScale() -> float:
    pass
def GetViewportOffset() -> typing.Tuple[float, float]:
    pass
def GetViewportSize() -> typing.Tuple[float, float]:
    pass
def GetWindowSize() -> typing.Tuple[float, float]:
    pass
def Image(relativePath: str, w: float, h: float) -> None:
    pass
def ImageScaled(relativePath: str, w: float, h: float) -> None:
    pass
def Indent(w: float = 0.0) -> None:
    pass
def IndentScaled(w: float = 0.0) -> None:
    pass
def InputFloat(arg0: str, arg1: float) -> typing.Tuple[bool, float]:
    pass
def InputInt(arg0: str, arg1: int) -> typing.Tuple[bool, int]:
    pass
def InputText(arg0: str, arg1: str) -> typing.Tuple[bool, str]:
    pass
def LabelText(arg0: str, arg1: str) -> None:
    pass
def LoadTexture(relativePath: str) -> None:
    pass
def NewLine() -> None:
    pass
def NextColumn() -> None:
    pass
def PopStyleColor(count: int = 1) -> None:
    pass
def PopStyleVar(count: int = 1) -> None:
    pass
def ProgressBar(fraction: float, w: float = -1.0, h: float = 0.0, overlay: str = '') -> None:
    pass
def ProgressBarScaled(fraction: float, w: float = -1.0, h: float = 0.0, overlay: str = '') -> None:
    pass
def PushStyleColor(arg0: int, arg1: float, arg2: float, arg3: float, arg4: float) -> None:
    pass
def PushStyleVar(arg0: int, arg1: float) -> None:
    pass
def PushStyleVar2(arg0: int, arg1: float, arg2: float) -> None:
    pass
def SameLine(offsetX: float = 0.0, spacing: float = -1.0) -> None:
    pass
def SameLineScaled(offsetX: float = 0.0, spacing: float = -1.0) -> None:
    pass
def Scale(arg0: float) -> float:
    pass
def Selectable(label: str, selected: bool = False) -> bool:
    pass
def Separator() -> None:
    pass
def SetColumnWidth(arg0: int, arg1: float) -> None:
    pass
def SetColumnWidthScaled(arg0: int, arg1: float) -> None:
    pass
def SetCursorPos(arg0: float, arg1: float) -> None:
    pass
def SetCursorPosScaled(arg0: float, arg1: float) -> None:
    pass
def SetNextWindowBgAlpha(arg0: float) -> None:
    pass
def SetNextWindowPos(arg0: float, arg1: float) -> None:
    pass
def SetNextWindowPosScaled(arg0: float, arg1: float) -> None:
    pass
def SetNextWindowSize(arg0: float, arg1: float) -> None:
    pass
def SetNextWindowSizeConstraints(arg0: float, arg1: float, arg2: float, arg3: float) -> None:
    pass
def SetNextWindowSizeConstraintsScaled(arg0: float, arg1: float, arg2: float, arg3: float) -> None:
    pass
def SetNextWindowSizeScaled(arg0: float, arg1: float) -> None:
    pass
def SliderFloat(arg0: str, arg1: float, arg2: float, arg3: float) -> typing.Tuple[bool, float]:
    pass
def SliderInt(arg0: str, arg1: int, arg2: int, arg3: int) -> typing.Tuple[bool, int]:
    pass
def SmallButton(arg0: str) -> bool:
    pass
def Spacing() -> None:
    pass
def SpacingScaled(count: float = 1.0) -> None:
    pass
def Text(arg0: str) -> None:
    pass
def TextColored(arg0: float, arg1: float, arg2: float, arg3: float, arg4: str) -> None:
    pass
def TreeNode(arg0: str) -> bool:
    pass
def TreePop() -> None:
    pass
def Unindent(w: float = 0.0) -> None:
    pass
def UnindentScaled(w: float = 0.0) -> None:
    pass
Col_Button = 21
Col_ButtonActive = 23
Col_ButtonHovered = 22
Col_ChildBg = 3
Col_FrameBg = 7
Col_FrameBgActive = 9
Col_FrameBgHovered = 8
Col_Header = 24
Col_HeaderActive = 26
Col_HeaderHovered = 25
Col_PlotHistogram = 45
Col_PopupBg = 4
Col_Text = 0
Col_TitleBg = 10
Col_TitleBgActive = 11
Col_WindowBg = 2
StyleVar_Alpha = 0
StyleVar_FramePadding = 11
StyleVar_FrameRounding = 12
StyleVar_IndentSpacing = 16
StyleVar_ItemInnerSpacing = 15
StyleVar_ItemSpacing = 14
StyleVar_WindowBorderSize = 4
StyleVar_WindowPadding = 2
StyleVar_WindowRounding = 3
WindowFlags_AlwaysAutoResize = 64
WindowFlags_NoBackground = 128
WindowFlags_NoBringToFrontOnFocus = 8192
WindowFlags_NoCollapse = 32
WindowFlags_NoDecoration = 43
WindowFlags_NoInputs = 197120
WindowFlags_NoMove = 4
WindowFlags_NoResize = 2
WindowFlags_NoSavedSettings = 256
WindowFlags_NoScrollWithMouse = 16
WindowFlags_NoScrollbar = 8
WindowFlags_NoTitleBar = 1
WindowFlags_None = 0
