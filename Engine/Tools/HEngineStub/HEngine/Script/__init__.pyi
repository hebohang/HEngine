"""Portable engine API; main-thread, session-handle and copy semantics."""
from __future__ import annotations
import HEngine.Script
import typing

__all__ = [
    "EmitEvent",
    "Entity",
    "EntityState",
    "InputState",
    "Log",
    "ReadEvent",
    "ReadSave",
    "WriteSave"
]


class Entity():
    def Apply(self, arg0: EntityState) -> None: ...
    def Destroy(self) -> None: ...
    def Find(self, arg0: str) -> Entity: ...
    def SetScriptEnabled(self, arg0: bool) -> None: ...
    def __init__(self) -> None: ...
    @property
    def Handle(self) -> int:
        """
        :type: int
        """
    @property
    def Input(self) -> InputState:
        """
        :type: InputState
        """
    @property
    def IsValid(self) -> bool:
        """
        :type: bool
        """
    @property
    def State(self) -> EntityState:
        """
        :type: EntityState
        """
    @State.setter
    def State(self, arg1: EntityState) -> None:
        pass
    @property
    def Texture(self) -> str:
        """
        :type: str
        """
    @Texture.setter
    def Texture(self, arg1: str) -> None:
        pass
    pass
class EntityState():
    def __init__(self) -> None: ...
    @property
    def Color(self) -> HEngine.vec4:
        """
        :type: HEngine.vec4
        """
    @Color.setter
    def Color(self, arg0: HEngine.vec4) -> None:
        pass
    @property
    def HasSprite(self) -> bool:
        """
        :type: bool
        """
    @HasSprite.setter
    def HasSprite(self, arg0: bool) -> None:
        pass
    @property
    def Position(self) -> HEngine.vec3:
        """
        :type: HEngine.vec3
        """
    @Position.setter
    def Position(self, arg0: HEngine.vec3) -> None:
        pass
    @property
    def Rotation(self) -> HEngine.vec3:
        """
        :type: HEngine.vec3
        """
    @Rotation.setter
    def Rotation(self, arg0: HEngine.vec3) -> None:
        pass
    @property
    def Scale(self) -> HEngine.vec3:
        """
        :type: HEngine.vec3
        """
    @Scale.setter
    def Scale(self, arg0: HEngine.vec3) -> None:
        pass
    pass
class InputState():
    @property
    def Movement(self) -> HEngine.vec2:
        """
        :type: HEngine.vec2
        """
    @property
    def Pointer(self) -> HEngine.vec2:
        """
        :type: HEngine.vec2
        """
    @property
    def PointerDown(self) -> bool:
        """
        :type: bool
        """
    @property
    def PressSequence(self) -> int:
        """
        :type: int
        """
    pass
def EmitEvent(arg0: str, arg1: str) -> None:
    pass
def Log(arg0: str) -> None:
    pass
def ReadEvent(arg0: str, arg1: int) -> tuple:
    pass
def ReadSave(arg0: str) -> str:
    pass
def WriteSave(arg0: str, arg1: str) -> None:
    pass
