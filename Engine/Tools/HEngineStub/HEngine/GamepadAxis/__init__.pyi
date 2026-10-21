from __future__ import annotations
import HEngine.GamepadAxis
import typing

__all__ = [
    "GamepadAxis",
    "LeftTrigger",
    "LeftX",
    "LeftY",
    "RightTrigger",
    "RightX",
    "RightY"
]


class GamepadAxis():
    """
    Members:

      LeftX

      LeftY

      RightX

      RightY

      LeftTrigger

      RightTrigger
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
    LeftTrigger: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.LeftTrigger: 4>
    LeftX: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.LeftX: 0>
    LeftY: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.LeftY: 1>
    RightTrigger: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.RightTrigger: 5>
    RightX: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.RightX: 2>
    RightY: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.RightY: 3>
    __members__: dict # value = {'LeftX': <GamepadAxis.LeftX: 0>, 'LeftY': <GamepadAxis.LeftY: 1>, 'RightX': <GamepadAxis.RightX: 2>, 'RightY': <GamepadAxis.RightY: 3>, 'LeftTrigger': <GamepadAxis.LeftTrigger: 4>, 'RightTrigger': <GamepadAxis.RightTrigger: 5>}
    pass
LeftTrigger: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.LeftTrigger: 4>
LeftX: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.LeftX: 0>
LeftY: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.LeftY: 1>
RightTrigger: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.RightTrigger: 5>
RightX: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.RightX: 2>
RightY: HEngine.GamepadAxis.GamepadAxis # value = <GamepadAxis.RightY: 3>
