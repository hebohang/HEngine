from __future__ import annotations
import HEngine.MouseCode
import typing

__all__ = [
    "Button0",
    "Button1",
    "Button2",
    "Button3",
    "Button4",
    "Button5",
    "Button6",
    "Button7",
    "ButtonLeft",
    "ButtonMiddle",
    "ButtonRight",
    "MouseCode"
]


class MouseCode():
    """
    Members:

      Button0

      Button1

      Button2

      Button3

      Button4

      Button5

      Button6

      Button7

      ButtonLeft

      ButtonRight

      ButtonMiddle
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
    Button0: HEngine.MouseCode.MouseCode # value = <MouseCode.Button0: 0>
    Button1: HEngine.MouseCode.MouseCode # value = <MouseCode.Button1: 1>
    Button2: HEngine.MouseCode.MouseCode # value = <MouseCode.Button2: 2>
    Button3: HEngine.MouseCode.MouseCode # value = <MouseCode.Button3: 3>
    Button4: HEngine.MouseCode.MouseCode # value = <MouseCode.Button4: 4>
    Button5: HEngine.MouseCode.MouseCode # value = <MouseCode.Button5: 5>
    Button6: HEngine.MouseCode.MouseCode # value = <MouseCode.Button6: 6>
    Button7: HEngine.MouseCode.MouseCode # value = <MouseCode.Button7: 7>
    ButtonLeft: HEngine.MouseCode.MouseCode # value = <MouseCode.Button0: 0>
    ButtonMiddle: HEngine.MouseCode.MouseCode # value = <MouseCode.Button2: 2>
    ButtonRight: HEngine.MouseCode.MouseCode # value = <MouseCode.Button1: 1>
    __members__: dict # value = {'Button0': <MouseCode.Button0: 0>, 'Button1': <MouseCode.Button1: 1>, 'Button2': <MouseCode.Button2: 2>, 'Button3': <MouseCode.Button3: 3>, 'Button4': <MouseCode.Button4: 4>, 'Button5': <MouseCode.Button5: 5>, 'Button6': <MouseCode.Button6: 6>, 'Button7': <MouseCode.Button7: 7>, 'ButtonLeft': <MouseCode.Button0: 0>, 'ButtonRight': <MouseCode.Button1: 1>, 'ButtonMiddle': <MouseCode.Button2: 2>}
    pass
Button0: HEngine.MouseCode.MouseCode # value = <MouseCode.Button0: 0>
Button1: HEngine.MouseCode.MouseCode # value = <MouseCode.Button1: 1>
Button2: HEngine.MouseCode.MouseCode # value = <MouseCode.Button2: 2>
Button3: HEngine.MouseCode.MouseCode # value = <MouseCode.Button3: 3>
Button4: HEngine.MouseCode.MouseCode # value = <MouseCode.Button4: 4>
Button5: HEngine.MouseCode.MouseCode # value = <MouseCode.Button5: 5>
Button6: HEngine.MouseCode.MouseCode # value = <MouseCode.Button6: 6>
Button7: HEngine.MouseCode.MouseCode # value = <MouseCode.Button7: 7>
ButtonLeft: HEngine.MouseCode.MouseCode # value = <MouseCode.Button0: 0>
ButtonMiddle: HEngine.MouseCode.MouseCode # value = <MouseCode.Button2: 2>
ButtonRight: HEngine.MouseCode.MouseCode # value = <MouseCode.Button1: 1>
