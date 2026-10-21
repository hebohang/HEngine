from __future__ import annotations
import HEngine.Input
import typing
import HEngine
import HEngine.KeyCode
import HEngine.MouseCode

__all__ = [
    "FindFirstConnectedGamepad",
    "GetGamepadState",
    "IsKeyPressed",
    "IsMouseButtonPressed"
]


def FindFirstConnectedGamepad() -> int:
    pass
def GetGamepadState(gamepadIndex: int = 0) -> HEngine.GamepadState:
    pass
def IsKeyPressed(arg0: HEngine.KeyCode.KeyCode) -> bool:
    pass
def IsMouseButtonPressed(arg0: HEngine.MouseCode.MouseCode) -> bool:
    pass
