from __future__ import annotations
import HEngine.RmlUI
import typing

__all__ = [
    "ClearRenderTransition",
    "ClickElement",
    "FocusElement",
    "GetClass",
    "GetPresentationDelta",
    "GetPresentationTime",
    "GetRenderCapabilities",
    "GetStyle",
    "Hide",
    "Load",
    "LoadMotionAsset",
    "Reload",
    "SetActive",
    "SetAttribute",
    "SetClass",
    "SetCustomMouseCursor",
    "SetMouseCursorVisible",
    "SetRenderTransition",
    "SetStyle",
    "SetText",
    "Show",
    "Unload"
]


def ClearRenderTransition() -> None:
    pass
def ClickElement(arg0: str, arg1: str) -> bool:
    pass
def FocusElement(arg0: str, arg1: str) -> bool:
    pass
def GetClass(arg0: str, arg1: str, arg2: str) -> object:
    pass
def GetPresentationDelta() -> float:
    pass
def GetPresentationTime() -> float:
    pass
def GetRenderCapabilities() -> dict:
    pass
def GetStyle(arg0: str, arg1: str, arg2: str) -> object:
    pass
def Hide(arg0: str) -> bool:
    pass
def Load(arg0: str) -> str:
    pass
def LoadMotionAsset(arg0: str) -> object:
    pass
def Reload(arg0: str) -> bool:
    pass
def SetActive(arg0: str) -> bool:
    pass
def SetAttribute(arg0: str, arg1: str, arg2: str, arg3: str) -> bool:
    pass
def SetClass(documentId: str, elementId: str, className: str, enabled: bool = True) -> bool:
    pass
def SetCustomMouseCursor(documentId: str, assetPath: str, hotspotX: int = 0, hotspotY: int = 0) -> bool:
    pass
def SetMouseCursorVisible(arg0: str, arg1: bool) -> bool:
    pass
def SetRenderTransition(effect: str, progress: float, reduce_motion: bool = False, reduce_flash: bool = False) -> None:
    pass
def SetStyle(arg0: str, arg1: str, arg2: str, arg3: str) -> bool:
    pass
def SetText(arg0: str, arg1: str, arg2: str) -> bool:
    pass
def Show(arg0: str) -> bool:
    pass
def Unload(arg0: str) -> bool:
    pass
