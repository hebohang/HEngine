from __future__ import annotations
import HEngine.Audio
import typing
import HEngine

__all__ = [
    "Attenuation",
    "Bus",
    "Disabled",
    "Exponential",
    "Gameplay",
    "GetBusVolume",
    "GetLastError",
    "GetLength",
    "GetMasterVolume",
    "GetPlaybackPosition",
    "Inverse",
    "IsDeviceAvailable",
    "IsPaused",
    "IsPlaying",
    "Linear",
    "Music",
    "Pause",
    "Play",
    "Play3D",
    "Resume",
    "Seek",
    "SetBusVolume",
    "SetCone",
    "SetDirection",
    "SetListener",
    "SetMasterVolume",
    "SetPan",
    "SetPitch",
    "SetPosition",
    "SetVelocity",
    "SetVolume",
    "Stop",
    "StopBus",
    "UI"
]


class Attenuation():
    """
    Members:

      Disabled

      Inverse

      Linear

      Exponential
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
    Disabled: HEngine.Audio.Attenuation # value = <Attenuation.Disabled: 0>
    Exponential: HEngine.Audio.Attenuation # value = <Attenuation.Exponential: 3>
    Inverse: HEngine.Audio.Attenuation # value = <Attenuation.Inverse: 1>
    Linear: HEngine.Audio.Attenuation # value = <Attenuation.Linear: 2>
    __members__: dict # value = {'Disabled': <Attenuation.Disabled: 0>, 'Inverse': <Attenuation.Inverse: 1>, 'Linear': <Attenuation.Linear: 2>, 'Exponential': <Attenuation.Exponential: 3>}
    pass
class Bus():
    """
    Members:

      Music

      Gameplay

      UI
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
    Gameplay: HEngine.Audio.Bus # value = <Bus.Gameplay: 1>
    Music: HEngine.Audio.Bus # value = <Bus.Music: 0>
    UI: HEngine.Audio.Bus # value = <Bus.UI: 2>
    __members__: dict # value = {'Music': <Bus.Music: 0>, 'Gameplay': <Bus.Gameplay: 1>, 'UI': <Bus.UI: 2>}
    pass
def GetBusVolume(arg0: Bus) -> float:
    pass
def GetLastError() -> str:
    pass
def GetLength(arg0: int) -> float:
    pass
def GetMasterVolume() -> float:
    pass
def GetPlaybackPosition(arg0: int) -> float:
    pass
def IsDeviceAvailable() -> bool:
    pass
def IsPaused(arg0: int) -> bool:
    pass
def IsPlaying(arg0: int) -> bool:
    pass
def Pause(arg0: int) -> bool:
    pass
def Play(path: str, bus: Bus = Bus.Gameplay, volume: float = 1.0, pitch: float = 1.0, loop: bool = False, stream: bool = False, pan: float = 0.0) -> int:
    pass
def Play3D(path: str, position: HEngine.vec3, bus: Bus = Bus.Gameplay, volume: float = 1.0, pitch: float = 1.0, min_distance: float = 1.0, max_distance: float = 50.0, rolloff: float = 1.0, loop: bool = False, stream: bool = False, attenuation: Attenuation = Attenuation.Inverse, doppler_factor: float = 1.0) -> int:
    pass
def Resume(arg0: int) -> bool:
    pass
def Seek(arg0: int, arg1: float) -> bool:
    pass
def SetBusVolume(arg0: Bus, arg1: float) -> None:
    pass
def SetCone(handle: int, inner_angle_radians: float, outer_angle_radians: float, outer_gain: float) -> bool:
    pass
def SetDirection(arg0: int, arg1: HEngine.vec3) -> bool:
    pass
def SetListener(position: HEngine.vec3, forward: HEngine.vec3 = ..., up: HEngine.vec3 = ..., velocity: HEngine.vec3 = ...) -> bool:
    """
    Sets listener 0 immediately. During scene updates, an active AudioListenerComponent (or the primary camera fallback) remains authoritative and overwrites this value.
    """
def SetMasterVolume(arg0: float) -> None:
    pass
def SetPan(arg0: int, arg1: float) -> bool:
    pass
def SetPitch(arg0: int, arg1: float) -> bool:
    pass
def SetPosition(arg0: int, arg1: HEngine.vec3) -> bool:
    pass
def SetVelocity(arg0: int, arg1: HEngine.vec3) -> bool:
    pass
def SetVolume(arg0: int, arg1: float) -> bool:
    pass
def Stop(arg0: int) -> bool:
    pass
def StopBus(arg0: Bus) -> None:
    pass
Disabled: HEngine.Audio.Attenuation # value = <Attenuation.Disabled: 0>
Exponential: HEngine.Audio.Attenuation # value = <Attenuation.Exponential: 3>
Gameplay: HEngine.Audio.Bus # value = <Bus.Gameplay: 1>
Inverse: HEngine.Audio.Attenuation # value = <Attenuation.Inverse: 1>
Linear: HEngine.Audio.Attenuation # value = <Attenuation.Linear: 2>
Music: HEngine.Audio.Bus # value = <Bus.Music: 0>
UI: HEngine.Audio.Bus # value = <Bus.UI: 2>
