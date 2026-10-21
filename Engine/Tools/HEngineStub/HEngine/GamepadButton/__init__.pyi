from __future__ import annotations
import HEngine.GamepadButton
import typing

__all__ = [
    "A",
    "B",
    "Back",
    "DPadDown",
    "DPadLeft",
    "DPadRight",
    "DPadUp",
    "GamepadButton",
    "Guide",
    "LeftBumper",
    "LeftThumb",
    "RightBumper",
    "RightThumb",
    "Start",
    "X",
    "Y"
]


class GamepadButton():
    """
    Members:

      A

      B

      X

      Y

      LeftBumper

      RightBumper

      Back

      Start

      Guide

      LeftThumb

      RightThumb

      DPadUp

      DPadRight

      DPadDown

      DPadLeft
    """
    def __eq__(self, other: object) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: int) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: int) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str:
        """
        :type: str
        """
    @property
    def value(self) -> int:
        """
        :type: int
        """
    A: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.A: 0>
    B: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.B: 1>
    Back: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.Back: 6>
    DPadDown: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.DPadDown: 13>
    DPadLeft: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.DPadLeft: 14>
    DPadRight: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.DPadRight: 12>
    DPadUp: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.DPadUp: 11>
    Guide: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.Guide: 8>
    LeftBumper: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.LeftBumper: 4>
    LeftThumb: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.LeftThumb: 9>
    RightBumper: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.RightBumper: 5>
    RightThumb: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.RightThumb: 10>
    Start: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.Start: 7>
    X: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.X: 2>
    Y: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.Y: 3>
    __members__: dict # value = {'A': <GamepadButton.A: 0>, 'B': <GamepadButton.B: 1>, 'X': <GamepadButton.X: 2>, 'Y': <GamepadButton.Y: 3>, 'LeftBumper': <GamepadButton.LeftBumper: 4>, 'RightBumper': <GamepadButton.RightBumper: 5>, 'Back': <GamepadButton.Back: 6>, 'Start': <GamepadButton.Start: 7>, 'Guide': <GamepadButton.Guide: 8>, 'LeftThumb': <GamepadButton.LeftThumb: 9>, 'RightThumb': <GamepadButton.RightThumb: 10>, 'DPadUp': <GamepadButton.DPadUp: 11>, 'DPadRight': <GamepadButton.DPadRight: 12>, 'DPadDown': <GamepadButton.DPadDown: 13>, 'DPadLeft': <GamepadButton.DPadLeft: 14>}
    pass
A: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.A: 0>
B: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.B: 1>
Back: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.Back: 6>
DPadDown: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.DPadDown: 13>
DPadLeft: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.DPadLeft: 14>
DPadRight: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.DPadRight: 12>
DPadUp: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.DPadUp: 11>
Guide: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.Guide: 8>
LeftBumper: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.LeftBumper: 4>
LeftThumb: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.LeftThumb: 9>
RightBumper: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.RightBumper: 5>
RightThumb: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.RightThumb: 10>
Start: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.Start: 7>
X: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.X: 2>
Y: HEngine.GamepadButton.GamepadButton # value = <GamepadButton.Y: 3>
