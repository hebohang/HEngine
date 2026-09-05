from __future__ import annotations
import HEngine.KeyCode
import typing

__all__ = [
    "A",
    "Apostrophe",
    "B",
    "Backslash",
    "Backspace",
    "C",
    "CapsLock",
    "Comma",
    "D",
    "D0",
    "D1",
    "D2",
    "D3",
    "D4",
    "D5",
    "D6",
    "D7",
    "D8",
    "D9",
    "Delete",
    "Down",
    "E",
    "End",
    "Enter",
    "Equal",
    "Escape",
    "F",
    "F1",
    "F10",
    "F11",
    "F12",
    "F13",
    "F14",
    "F15",
    "F16",
    "F17",
    "F18",
    "F19",
    "F2",
    "F20",
    "F21",
    "F22",
    "F23",
    "F24",
    "F25",
    "F3",
    "F4",
    "F5",
    "F6",
    "F7",
    "F8",
    "F9",
    "G",
    "GraveAccent",
    "H",
    "Home",
    "I",
    "Insert",
    "J",
    "K",
    "KP0",
    "KP1",
    "KP2",
    "KP3",
    "KP4",
    "KP5",
    "KP6",
    "KP7",
    "KP8",
    "KP9",
    "KPAdd",
    "KPDecimal",
    "KPDivide",
    "KPEnter",
    "KPEqual",
    "KPMultiply",
    "KPSubtract",
    "KeyCode",
    "L",
    "Left",
    "LeftAlt",
    "LeftBracket",
    "LeftControl",
    "LeftShift",
    "LeftSuper",
    "M",
    "Menu",
    "Minus",
    "N",
    "NumLock",
    "O",
    "P",
    "PageDown",
    "PageUp",
    "Pause",
    "Period",
    "PrintScreen",
    "Q",
    "R",
    "Right",
    "RightAlt",
    "RightBracket",
    "RightControl",
    "RightShift",
    "RightSuper",
    "S",
    "ScrollLock",
    "Semicolon",
    "Slash",
    "Space",
    "T",
    "Tab",
    "U",
    "Up",
    "V",
    "W",
    "World1",
    "World2",
    "X",
    "Y",
    "Z"
]


class KeyCode():
    """
    Members:

      Space

      Apostrophe

      Comma

      Minus

      Period

      Slash

      D0

      D1

      D2

      D3

      D4

      D5

      D6

      D7

      D8

      D9

      Semicolon

      Equal

      A

      B

      C

      D

      E

      F

      G

      H

      I

      J

      K

      L

      M

      N

      O

      P

      Q

      R

      S

      T

      U

      V

      W

      X

      Y

      Z

      LeftBracket

      Backslash

      RightBracket

      GraveAccent

      World1

      World2

      Escape

      Enter

      Tab

      Backspace

      Insert

      Delete

      Right

      Left

      Down

      Up

      PageUp

      PageDown

      Home

      End

      CapsLock

      ScrollLock

      NumLock

      PrintScreen

      Pause

      F1

      F2

      F3

      F4

      F5

      F6

      F7

      F8

      F9

      F10

      F11

      F12

      F13

      F14

      F15

      F16

      F17

      F18

      F19

      F20

      F21

      F22

      F23

      F24

      F25

      KP0

      KP1

      KP2

      KP3

      KP4

      KP5

      KP6

      KP7

      KP8

      KP9

      KPDecimal

      KPDivide

      KPMultiply

      KPSubtract

      KPAdd

      KPEnter

      KPEqual

      LeftShift

      LeftControl

      LeftAlt

      LeftSuper

      RightShift

      RightControl

      RightAlt

      RightSuper

      Menu
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
    A: HEngine.KeyCode.KeyCode # value = <KeyCode.A: 65>
    Apostrophe: HEngine.KeyCode.KeyCode # value = <KeyCode.Apostrophe: 39>
    B: HEngine.KeyCode.KeyCode # value = <KeyCode.B: 66>
    Backslash: HEngine.KeyCode.KeyCode # value = <KeyCode.Backslash: 92>
    Backspace: HEngine.KeyCode.KeyCode # value = <KeyCode.Backspace: 259>
    C: HEngine.KeyCode.KeyCode # value = <KeyCode.C: 67>
    CapsLock: HEngine.KeyCode.KeyCode # value = <KeyCode.CapsLock: 280>
    Comma: HEngine.KeyCode.KeyCode # value = <KeyCode.Comma: 44>
    D: HEngine.KeyCode.KeyCode # value = <KeyCode.D: 68>
    D0: HEngine.KeyCode.KeyCode # value = <KeyCode.D0: 48>
    D1: HEngine.KeyCode.KeyCode # value = <KeyCode.D1: 49>
    D2: HEngine.KeyCode.KeyCode # value = <KeyCode.D2: 50>
    D3: HEngine.KeyCode.KeyCode # value = <KeyCode.D3: 51>
    D4: HEngine.KeyCode.KeyCode # value = <KeyCode.D4: 52>
    D5: HEngine.KeyCode.KeyCode # value = <KeyCode.D5: 53>
    D6: HEngine.KeyCode.KeyCode # value = <KeyCode.D6: 54>
    D7: HEngine.KeyCode.KeyCode # value = <KeyCode.D7: 55>
    D8: HEngine.KeyCode.KeyCode # value = <KeyCode.D8: 56>
    D9: HEngine.KeyCode.KeyCode # value = <KeyCode.D9: 57>
    Delete: HEngine.KeyCode.KeyCode # value = <KeyCode.Delete: 261>
    Down: HEngine.KeyCode.KeyCode # value = <KeyCode.Down: 264>
    E: HEngine.KeyCode.KeyCode # value = <KeyCode.E: 69>
    End: HEngine.KeyCode.KeyCode # value = <KeyCode.End: 269>
    Enter: HEngine.KeyCode.KeyCode # value = <KeyCode.Enter: 257>
    Equal: HEngine.KeyCode.KeyCode # value = <KeyCode.Equal: 61>
    Escape: HEngine.KeyCode.KeyCode # value = <KeyCode.Escape: 256>
    F: HEngine.KeyCode.KeyCode # value = <KeyCode.F: 70>
    F1: HEngine.KeyCode.KeyCode # value = <KeyCode.F1: 290>
    F10: HEngine.KeyCode.KeyCode # value = <KeyCode.F10: 299>
    F11: HEngine.KeyCode.KeyCode # value = <KeyCode.F11: 300>
    F12: HEngine.KeyCode.KeyCode # value = <KeyCode.F12: 301>
    F13: HEngine.KeyCode.KeyCode # value = <KeyCode.F13: 302>
    F14: HEngine.KeyCode.KeyCode # value = <KeyCode.F14: 303>
    F15: HEngine.KeyCode.KeyCode # value = <KeyCode.F15: 304>
    F16: HEngine.KeyCode.KeyCode # value = <KeyCode.F16: 305>
    F17: HEngine.KeyCode.KeyCode # value = <KeyCode.F17: 306>
    F18: HEngine.KeyCode.KeyCode # value = <KeyCode.F18: 307>
    F19: HEngine.KeyCode.KeyCode # value = <KeyCode.F19: 308>
    F2: HEngine.KeyCode.KeyCode # value = <KeyCode.F2: 291>
    F20: HEngine.KeyCode.KeyCode # value = <KeyCode.F20: 309>
    F21: HEngine.KeyCode.KeyCode # value = <KeyCode.F21: 310>
    F22: HEngine.KeyCode.KeyCode # value = <KeyCode.F22: 311>
    F23: HEngine.KeyCode.KeyCode # value = <KeyCode.F23: 312>
    F24: HEngine.KeyCode.KeyCode # value = <KeyCode.F24: 313>
    F25: HEngine.KeyCode.KeyCode # value = <KeyCode.F25: 314>
    F3: HEngine.KeyCode.KeyCode # value = <KeyCode.F3: 292>
    F4: HEngine.KeyCode.KeyCode # value = <KeyCode.F4: 293>
    F5: HEngine.KeyCode.KeyCode # value = <KeyCode.F5: 294>
    F6: HEngine.KeyCode.KeyCode # value = <KeyCode.F6: 295>
    F7: HEngine.KeyCode.KeyCode # value = <KeyCode.F7: 296>
    F8: HEngine.KeyCode.KeyCode # value = <KeyCode.F8: 297>
    F9: HEngine.KeyCode.KeyCode # value = <KeyCode.F9: 298>
    G: HEngine.KeyCode.KeyCode # value = <KeyCode.G: 71>
    GraveAccent: HEngine.KeyCode.KeyCode # value = <KeyCode.GraveAccent: 96>
    H: HEngine.KeyCode.KeyCode # value = <KeyCode.H: 72>
    Home: HEngine.KeyCode.KeyCode # value = <KeyCode.Home: 268>
    I: HEngine.KeyCode.KeyCode # value = <KeyCode.I: 73>
    Insert: HEngine.KeyCode.KeyCode # value = <KeyCode.Insert: 260>
    J: HEngine.KeyCode.KeyCode # value = <KeyCode.J: 74>
    K: HEngine.KeyCode.KeyCode # value = <KeyCode.K: 75>
    KP0: HEngine.KeyCode.KeyCode # value = <KeyCode.KP0: 320>
    KP1: HEngine.KeyCode.KeyCode # value = <KeyCode.KP1: 321>
    KP2: HEngine.KeyCode.KeyCode # value = <KeyCode.KP2: 322>
    KP3: HEngine.KeyCode.KeyCode # value = <KeyCode.KP3: 323>
    KP4: HEngine.KeyCode.KeyCode # value = <KeyCode.KP4: 324>
    KP5: HEngine.KeyCode.KeyCode # value = <KeyCode.KP5: 325>
    KP6: HEngine.KeyCode.KeyCode # value = <KeyCode.KP6: 326>
    KP7: HEngine.KeyCode.KeyCode # value = <KeyCode.KP7: 327>
    KP8: HEngine.KeyCode.KeyCode # value = <KeyCode.KP8: 328>
    KP9: HEngine.KeyCode.KeyCode # value = <KeyCode.KP9: 329>
    KPAdd: HEngine.KeyCode.KeyCode # value = <KeyCode.KPAdd: 334>
    KPDecimal: HEngine.KeyCode.KeyCode # value = <KeyCode.KPDecimal: 330>
    KPDivide: HEngine.KeyCode.KeyCode # value = <KeyCode.KPDivide: 331>
    KPEnter: HEngine.KeyCode.KeyCode # value = <KeyCode.KPEnter: 335>
    KPEqual: HEngine.KeyCode.KeyCode # value = <KeyCode.KPEqual: 336>
    KPMultiply: HEngine.KeyCode.KeyCode # value = <KeyCode.KPMultiply: 332>
    KPSubtract: HEngine.KeyCode.KeyCode # value = <KeyCode.KPSubtract: 333>
    L: HEngine.KeyCode.KeyCode # value = <KeyCode.L: 76>
    Left: HEngine.KeyCode.KeyCode # value = <KeyCode.Left: 263>
    LeftAlt: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftAlt: 342>
    LeftBracket: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftBracket: 91>
    LeftControl: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftControl: 341>
    LeftShift: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftShift: 340>
    LeftSuper: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftSuper: 343>
    M: HEngine.KeyCode.KeyCode # value = <KeyCode.M: 77>
    Menu: HEngine.KeyCode.KeyCode # value = <KeyCode.Menu: 348>
    Minus: HEngine.KeyCode.KeyCode # value = <KeyCode.Minus: 45>
    N: HEngine.KeyCode.KeyCode # value = <KeyCode.N: 78>
    NumLock: HEngine.KeyCode.KeyCode # value = <KeyCode.NumLock: 282>
    O: HEngine.KeyCode.KeyCode # value = <KeyCode.O: 79>
    P: HEngine.KeyCode.KeyCode # value = <KeyCode.P: 80>
    PageDown: HEngine.KeyCode.KeyCode # value = <KeyCode.PageDown: 267>
    PageUp: HEngine.KeyCode.KeyCode # value = <KeyCode.PageUp: 266>
    Pause: HEngine.KeyCode.KeyCode # value = <KeyCode.Pause: 284>
    Period: HEngine.KeyCode.KeyCode # value = <KeyCode.Period: 46>
    PrintScreen: HEngine.KeyCode.KeyCode # value = <KeyCode.PrintScreen: 283>
    Q: HEngine.KeyCode.KeyCode # value = <KeyCode.Q: 81>
    R: HEngine.KeyCode.KeyCode # value = <KeyCode.R: 82>
    Right: HEngine.KeyCode.KeyCode # value = <KeyCode.Right: 262>
    RightAlt: HEngine.KeyCode.KeyCode # value = <KeyCode.RightAlt: 346>
    RightBracket: HEngine.KeyCode.KeyCode # value = <KeyCode.RightBracket: 93>
    RightControl: HEngine.KeyCode.KeyCode # value = <KeyCode.RightControl: 345>
    RightShift: HEngine.KeyCode.KeyCode # value = <KeyCode.RightShift: 344>
    RightSuper: HEngine.KeyCode.KeyCode # value = <KeyCode.RightSuper: 347>
    S: HEngine.KeyCode.KeyCode # value = <KeyCode.S: 83>
    ScrollLock: HEngine.KeyCode.KeyCode # value = <KeyCode.ScrollLock: 281>
    Semicolon: HEngine.KeyCode.KeyCode # value = <KeyCode.Semicolon: 59>
    Slash: HEngine.KeyCode.KeyCode # value = <KeyCode.Slash: 47>
    Space: HEngine.KeyCode.KeyCode # value = <KeyCode.Space: 32>
    T: HEngine.KeyCode.KeyCode # value = <KeyCode.T: 84>
    Tab: HEngine.KeyCode.KeyCode # value = <KeyCode.Tab: 258>
    U: HEngine.KeyCode.KeyCode # value = <KeyCode.U: 85>
    Up: HEngine.KeyCode.KeyCode # value = <KeyCode.Up: 265>
    V: HEngine.KeyCode.KeyCode # value = <KeyCode.V: 86>
    W: HEngine.KeyCode.KeyCode # value = <KeyCode.W: 87>
    World1: HEngine.KeyCode.KeyCode # value = <KeyCode.World1: 161>
    World2: HEngine.KeyCode.KeyCode # value = <KeyCode.World2: 162>
    X: HEngine.KeyCode.KeyCode # value = <KeyCode.X: 88>
    Y: HEngine.KeyCode.KeyCode # value = <KeyCode.Y: 89>
    Z: HEngine.KeyCode.KeyCode # value = <KeyCode.Z: 90>
    __members__: dict # value = {'Space': <KeyCode.Space: 32>, 'Apostrophe': <KeyCode.Apostrophe: 39>, 'Comma': <KeyCode.Comma: 44>, 'Minus': <KeyCode.Minus: 45>, 'Period': <KeyCode.Period: 46>, 'Slash': <KeyCode.Slash: 47>, 'D0': <KeyCode.D0: 48>, 'D1': <KeyCode.D1: 49>, 'D2': <KeyCode.D2: 50>, 'D3': <KeyCode.D3: 51>, 'D4': <KeyCode.D4: 52>, 'D5': <KeyCode.D5: 53>, 'D6': <KeyCode.D6: 54>, 'D7': <KeyCode.D7: 55>, 'D8': <KeyCode.D8: 56>, 'D9': <KeyCode.D9: 57>, 'Semicolon': <KeyCode.Semicolon: 59>, 'Equal': <KeyCode.Equal: 61>, 'A': <KeyCode.A: 65>, 'B': <KeyCode.B: 66>, 'C': <KeyCode.C: 67>, 'D': <KeyCode.D: 68>, 'E': <KeyCode.E: 69>, 'F': <KeyCode.F: 70>, 'G': <KeyCode.G: 71>, 'H': <KeyCode.H: 72>, 'I': <KeyCode.I: 73>, 'J': <KeyCode.J: 74>, 'K': <KeyCode.K: 75>, 'L': <KeyCode.L: 76>, 'M': <KeyCode.M: 77>, 'N': <KeyCode.N: 78>, 'O': <KeyCode.O: 79>, 'P': <KeyCode.P: 80>, 'Q': <KeyCode.Q: 81>, 'R': <KeyCode.R: 82>, 'S': <KeyCode.S: 83>, 'T': <KeyCode.T: 84>, 'U': <KeyCode.U: 85>, 'V': <KeyCode.V: 86>, 'W': <KeyCode.W: 87>, 'X': <KeyCode.X: 88>, 'Y': <KeyCode.Y: 89>, 'Z': <KeyCode.Z: 90>, 'LeftBracket': <KeyCode.LeftBracket: 91>, 'Backslash': <KeyCode.Backslash: 92>, 'RightBracket': <KeyCode.RightBracket: 93>, 'GraveAccent': <KeyCode.GraveAccent: 96>, 'World1': <KeyCode.World1: 161>, 'World2': <KeyCode.World2: 162>, 'Escape': <KeyCode.Escape: 256>, 'Enter': <KeyCode.Enter: 257>, 'Tab': <KeyCode.Tab: 258>, 'Backspace': <KeyCode.Backspace: 259>, 'Insert': <KeyCode.Insert: 260>, 'Delete': <KeyCode.Delete: 261>, 'Right': <KeyCode.Right: 262>, 'Left': <KeyCode.Left: 263>, 'Down': <KeyCode.Down: 264>, 'Up': <KeyCode.Up: 265>, 'PageUp': <KeyCode.PageUp: 266>, 'PageDown': <KeyCode.PageDown: 267>, 'Home': <KeyCode.Home: 268>, 'End': <KeyCode.End: 269>, 'CapsLock': <KeyCode.CapsLock: 280>, 'ScrollLock': <KeyCode.ScrollLock: 281>, 'NumLock': <KeyCode.NumLock: 282>, 'PrintScreen': <KeyCode.PrintScreen: 283>, 'Pause': <KeyCode.Pause: 284>, 'F1': <KeyCode.F1: 290>, 'F2': <KeyCode.F2: 291>, 'F3': <KeyCode.F3: 292>, 'F4': <KeyCode.F4: 293>, 'F5': <KeyCode.F5: 294>, 'F6': <KeyCode.F6: 295>, 'F7': <KeyCode.F7: 296>, 'F8': <KeyCode.F8: 297>, 'F9': <KeyCode.F9: 298>, 'F10': <KeyCode.F10: 299>, 'F11': <KeyCode.F11: 300>, 'F12': <KeyCode.F12: 301>, 'F13': <KeyCode.F13: 302>, 'F14': <KeyCode.F14: 303>, 'F15': <KeyCode.F15: 304>, 'F16': <KeyCode.F16: 305>, 'F17': <KeyCode.F17: 306>, 'F18': <KeyCode.F18: 307>, 'F19': <KeyCode.F19: 308>, 'F20': <KeyCode.F20: 309>, 'F21': <KeyCode.F21: 310>, 'F22': <KeyCode.F22: 311>, 'F23': <KeyCode.F23: 312>, 'F24': <KeyCode.F24: 313>, 'F25': <KeyCode.F25: 314>, 'KP0': <KeyCode.KP0: 320>, 'KP1': <KeyCode.KP1: 321>, 'KP2': <KeyCode.KP2: 322>, 'KP3': <KeyCode.KP3: 323>, 'KP4': <KeyCode.KP4: 324>, 'KP5': <KeyCode.KP5: 325>, 'KP6': <KeyCode.KP6: 326>, 'KP7': <KeyCode.KP7: 327>, 'KP8': <KeyCode.KP8: 328>, 'KP9': <KeyCode.KP9: 329>, 'KPDecimal': <KeyCode.KPDecimal: 330>, 'KPDivide': <KeyCode.KPDivide: 331>, 'KPMultiply': <KeyCode.KPMultiply: 332>, 'KPSubtract': <KeyCode.KPSubtract: 333>, 'KPAdd': <KeyCode.KPAdd: 334>, 'KPEnter': <KeyCode.KPEnter: 335>, 'KPEqual': <KeyCode.KPEqual: 336>, 'LeftShift': <KeyCode.LeftShift: 340>, 'LeftControl': <KeyCode.LeftControl: 341>, 'LeftAlt': <KeyCode.LeftAlt: 342>, 'LeftSuper': <KeyCode.LeftSuper: 343>, 'RightShift': <KeyCode.RightShift: 344>, 'RightControl': <KeyCode.RightControl: 345>, 'RightAlt': <KeyCode.RightAlt: 346>, 'RightSuper': <KeyCode.RightSuper: 347>, 'Menu': <KeyCode.Menu: 348>}
    pass
A: HEngine.KeyCode.KeyCode # value = <KeyCode.A: 65>
Apostrophe: HEngine.KeyCode.KeyCode # value = <KeyCode.Apostrophe: 39>
B: HEngine.KeyCode.KeyCode # value = <KeyCode.B: 66>
Backslash: HEngine.KeyCode.KeyCode # value = <KeyCode.Backslash: 92>
Backspace: HEngine.KeyCode.KeyCode # value = <KeyCode.Backspace: 259>
C: HEngine.KeyCode.KeyCode # value = <KeyCode.C: 67>
CapsLock: HEngine.KeyCode.KeyCode # value = <KeyCode.CapsLock: 280>
Comma: HEngine.KeyCode.KeyCode # value = <KeyCode.Comma: 44>
D: HEngine.KeyCode.KeyCode # value = <KeyCode.D: 68>
D0: HEngine.KeyCode.KeyCode # value = <KeyCode.D0: 48>
D1: HEngine.KeyCode.KeyCode # value = <KeyCode.D1: 49>
D2: HEngine.KeyCode.KeyCode # value = <KeyCode.D2: 50>
D3: HEngine.KeyCode.KeyCode # value = <KeyCode.D3: 51>
D4: HEngine.KeyCode.KeyCode # value = <KeyCode.D4: 52>
D5: HEngine.KeyCode.KeyCode # value = <KeyCode.D5: 53>
D6: HEngine.KeyCode.KeyCode # value = <KeyCode.D6: 54>
D7: HEngine.KeyCode.KeyCode # value = <KeyCode.D7: 55>
D8: HEngine.KeyCode.KeyCode # value = <KeyCode.D8: 56>
D9: HEngine.KeyCode.KeyCode # value = <KeyCode.D9: 57>
Delete: HEngine.KeyCode.KeyCode # value = <KeyCode.Delete: 261>
Down: HEngine.KeyCode.KeyCode # value = <KeyCode.Down: 264>
E: HEngine.KeyCode.KeyCode # value = <KeyCode.E: 69>
End: HEngine.KeyCode.KeyCode # value = <KeyCode.End: 269>
Enter: HEngine.KeyCode.KeyCode # value = <KeyCode.Enter: 257>
Equal: HEngine.KeyCode.KeyCode # value = <KeyCode.Equal: 61>
Escape: HEngine.KeyCode.KeyCode # value = <KeyCode.Escape: 256>
F: HEngine.KeyCode.KeyCode # value = <KeyCode.F: 70>
F1: HEngine.KeyCode.KeyCode # value = <KeyCode.F1: 290>
F10: HEngine.KeyCode.KeyCode # value = <KeyCode.F10: 299>
F11: HEngine.KeyCode.KeyCode # value = <KeyCode.F11: 300>
F12: HEngine.KeyCode.KeyCode # value = <KeyCode.F12: 301>
F13: HEngine.KeyCode.KeyCode # value = <KeyCode.F13: 302>
F14: HEngine.KeyCode.KeyCode # value = <KeyCode.F14: 303>
F15: HEngine.KeyCode.KeyCode # value = <KeyCode.F15: 304>
F16: HEngine.KeyCode.KeyCode # value = <KeyCode.F16: 305>
F17: HEngine.KeyCode.KeyCode # value = <KeyCode.F17: 306>
F18: HEngine.KeyCode.KeyCode # value = <KeyCode.F18: 307>
F19: HEngine.KeyCode.KeyCode # value = <KeyCode.F19: 308>
F2: HEngine.KeyCode.KeyCode # value = <KeyCode.F2: 291>
F20: HEngine.KeyCode.KeyCode # value = <KeyCode.F20: 309>
F21: HEngine.KeyCode.KeyCode # value = <KeyCode.F21: 310>
F22: HEngine.KeyCode.KeyCode # value = <KeyCode.F22: 311>
F23: HEngine.KeyCode.KeyCode # value = <KeyCode.F23: 312>
F24: HEngine.KeyCode.KeyCode # value = <KeyCode.F24: 313>
F25: HEngine.KeyCode.KeyCode # value = <KeyCode.F25: 314>
F3: HEngine.KeyCode.KeyCode # value = <KeyCode.F3: 292>
F4: HEngine.KeyCode.KeyCode # value = <KeyCode.F4: 293>
F5: HEngine.KeyCode.KeyCode # value = <KeyCode.F5: 294>
F6: HEngine.KeyCode.KeyCode # value = <KeyCode.F6: 295>
F7: HEngine.KeyCode.KeyCode # value = <KeyCode.F7: 296>
F8: HEngine.KeyCode.KeyCode # value = <KeyCode.F8: 297>
F9: HEngine.KeyCode.KeyCode # value = <KeyCode.F9: 298>
G: HEngine.KeyCode.KeyCode # value = <KeyCode.G: 71>
GraveAccent: HEngine.KeyCode.KeyCode # value = <KeyCode.GraveAccent: 96>
H: HEngine.KeyCode.KeyCode # value = <KeyCode.H: 72>
Home: HEngine.KeyCode.KeyCode # value = <KeyCode.Home: 268>
I: HEngine.KeyCode.KeyCode # value = <KeyCode.I: 73>
Insert: HEngine.KeyCode.KeyCode # value = <KeyCode.Insert: 260>
J: HEngine.KeyCode.KeyCode # value = <KeyCode.J: 74>
K: HEngine.KeyCode.KeyCode # value = <KeyCode.K: 75>
KP0: HEngine.KeyCode.KeyCode # value = <KeyCode.KP0: 320>
KP1: HEngine.KeyCode.KeyCode # value = <KeyCode.KP1: 321>
KP2: HEngine.KeyCode.KeyCode # value = <KeyCode.KP2: 322>
KP3: HEngine.KeyCode.KeyCode # value = <KeyCode.KP3: 323>
KP4: HEngine.KeyCode.KeyCode # value = <KeyCode.KP4: 324>
KP5: HEngine.KeyCode.KeyCode # value = <KeyCode.KP5: 325>
KP6: HEngine.KeyCode.KeyCode # value = <KeyCode.KP6: 326>
KP7: HEngine.KeyCode.KeyCode # value = <KeyCode.KP7: 327>
KP8: HEngine.KeyCode.KeyCode # value = <KeyCode.KP8: 328>
KP9: HEngine.KeyCode.KeyCode # value = <KeyCode.KP9: 329>
KPAdd: HEngine.KeyCode.KeyCode # value = <KeyCode.KPAdd: 334>
KPDecimal: HEngine.KeyCode.KeyCode # value = <KeyCode.KPDecimal: 330>
KPDivide: HEngine.KeyCode.KeyCode # value = <KeyCode.KPDivide: 331>
KPEnter: HEngine.KeyCode.KeyCode # value = <KeyCode.KPEnter: 335>
KPEqual: HEngine.KeyCode.KeyCode # value = <KeyCode.KPEqual: 336>
KPMultiply: HEngine.KeyCode.KeyCode # value = <KeyCode.KPMultiply: 332>
KPSubtract: HEngine.KeyCode.KeyCode # value = <KeyCode.KPSubtract: 333>
L: HEngine.KeyCode.KeyCode # value = <KeyCode.L: 76>
Left: HEngine.KeyCode.KeyCode # value = <KeyCode.Left: 263>
LeftAlt: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftAlt: 342>
LeftBracket: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftBracket: 91>
LeftControl: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftControl: 341>
LeftShift: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftShift: 340>
LeftSuper: HEngine.KeyCode.KeyCode # value = <KeyCode.LeftSuper: 343>
M: HEngine.KeyCode.KeyCode # value = <KeyCode.M: 77>
Menu: HEngine.KeyCode.KeyCode # value = <KeyCode.Menu: 348>
Minus: HEngine.KeyCode.KeyCode # value = <KeyCode.Minus: 45>
N: HEngine.KeyCode.KeyCode # value = <KeyCode.N: 78>
NumLock: HEngine.KeyCode.KeyCode # value = <KeyCode.NumLock: 282>
O: HEngine.KeyCode.KeyCode # value = <KeyCode.O: 79>
P: HEngine.KeyCode.KeyCode # value = <KeyCode.P: 80>
PageDown: HEngine.KeyCode.KeyCode # value = <KeyCode.PageDown: 267>
PageUp: HEngine.KeyCode.KeyCode # value = <KeyCode.PageUp: 266>
Pause: HEngine.KeyCode.KeyCode # value = <KeyCode.Pause: 284>
Period: HEngine.KeyCode.KeyCode # value = <KeyCode.Period: 46>
PrintScreen: HEngine.KeyCode.KeyCode # value = <KeyCode.PrintScreen: 283>
Q: HEngine.KeyCode.KeyCode # value = <KeyCode.Q: 81>
R: HEngine.KeyCode.KeyCode # value = <KeyCode.R: 82>
Right: HEngine.KeyCode.KeyCode # value = <KeyCode.Right: 262>
RightAlt: HEngine.KeyCode.KeyCode # value = <KeyCode.RightAlt: 346>
RightBracket: HEngine.KeyCode.KeyCode # value = <KeyCode.RightBracket: 93>
RightControl: HEngine.KeyCode.KeyCode # value = <KeyCode.RightControl: 345>
RightShift: HEngine.KeyCode.KeyCode # value = <KeyCode.RightShift: 344>
RightSuper: HEngine.KeyCode.KeyCode # value = <KeyCode.RightSuper: 347>
S: HEngine.KeyCode.KeyCode # value = <KeyCode.S: 83>
ScrollLock: HEngine.KeyCode.KeyCode # value = <KeyCode.ScrollLock: 281>
Semicolon: HEngine.KeyCode.KeyCode # value = <KeyCode.Semicolon: 59>
Slash: HEngine.KeyCode.KeyCode # value = <KeyCode.Slash: 47>
Space: HEngine.KeyCode.KeyCode # value = <KeyCode.Space: 32>
T: HEngine.KeyCode.KeyCode # value = <KeyCode.T: 84>
Tab: HEngine.KeyCode.KeyCode # value = <KeyCode.Tab: 258>
U: HEngine.KeyCode.KeyCode # value = <KeyCode.U: 85>
Up: HEngine.KeyCode.KeyCode # value = <KeyCode.Up: 265>
V: HEngine.KeyCode.KeyCode # value = <KeyCode.V: 86>
W: HEngine.KeyCode.KeyCode # value = <KeyCode.W: 87>
World1: HEngine.KeyCode.KeyCode # value = <KeyCode.World1: 161>
World2: HEngine.KeyCode.KeyCode # value = <KeyCode.World2: 162>
X: HEngine.KeyCode.KeyCode # value = <KeyCode.X: 88>
Y: HEngine.KeyCode.KeyCode # value = <KeyCode.Y: 89>
Z: HEngine.KeyCode.KeyCode # value = <KeyCode.Z: 90>
