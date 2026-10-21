from __future__ import annotations
import HEngine.GM
import typing

__all__ = [
    "Discover",
    "Execute",
    "IsCurrentSessionTainted",
    "RegisterCommand",
    "RegisterStateDomain",
    "ReloadProjectCommands",
    "UnregisterOwner"
]


def Discover() -> object:
    pass
def Execute(command: str, arguments: dict = {}, confirm: bool = False) -> object:
    pass
def IsCurrentSessionTainted() -> bool:
    pass
def RegisterCommand(owner: str, descriptor: dict, handler: function) -> bool:
    pass
def RegisterStateDomain(owner: str, descriptor: dict, snapshot: function, commit: function, availability: object = None, restore: object = None, refresh: object = None, mark_tainted: object = None) -> bool:
    pass
def ReloadProjectCommands() -> None:
    pass
def UnregisterOwner(arg0: str) -> int:
    pass
