from __future__ import annotations
import HEngine.VFX
import typing

__all__ = [
    "Describe",
    "GetEffectPosition",
    "GetStats",
    "GetStatsSnapshot",
    "IsEffectPlaying",
    "IsGpuParticleCapable",
    "IsGpuParticlesEnabled",
    "LoadEffect",
    "PlayEffect",
    "PlayEffectAdvanced",
    "PlaySeeded",
    "ReloadEffect",
    "SetColor",
    "SetDynamicInput",
    "SetEffectTransform",
    "SetEffectTransforms",
    "SetGpuParticlesEnabled",
    "SetParameter",
    "SetPaused",
    "SetQualityTier",
    "SetSpeed",
    "StopAll",
    "StopEffect",
    "ValidateAllEffectPackages",
    "ValidateEffectPackages"
]


def Describe() -> object:
    pass
def GetEffectPosition(handle: int) -> tuple:
    pass
def GetStats() -> dict:
    pass
def GetStatsSnapshot() -> dict:
    pass
def IsEffectPlaying(handle: int) -> bool:
    pass
def IsGpuParticleCapable() -> bool:
    pass
def IsGpuParticlesEnabled() -> bool:
    pass
def LoadEffect(relativePath: str) -> bool:
    pass
def PlayEffect(relativePath: str, x: float, y: float, z: float) -> int:
    pass
def PlayEffectAdvanced(relativePath: str, x: float, y: float, z: float, fx: float, fy: float, fz: float, scale: float = 1.0, speed: float = 1.0, category: int = 1) -> int:
    pass
def PlaySeeded(relativePath: str, x: float, y: float, z: float, seed: int, scale: float = 1.0, speed: float = 1.0) -> int:
    pass
def ReloadEffect(relativePath: str) -> bool:
    pass
def SetColor(handle: int, r: float, g: float, b: float, a: float = 1.0) -> bool:
    pass
def SetDynamicInput(handle: int, index: int, value: float) -> bool:
    pass
def SetEffectTransform(handle: int, x: float, y: float, z: float, fx: float, fy: float, fz: float) -> bool:
    pass
def SetEffectTransforms(transforms: typing.Iterable) -> tuple:
    pass
def SetGpuParticlesEnabled(enabled: bool) -> bool:
    pass
def SetParameter(handle: int, name: str, value: float) -> bool:
    pass
def SetPaused(handle: int, paused: bool) -> bool:
    pass
def SetQualityTier(qualityTier: int) -> None:
    pass
def SetSpeed(handle: int, speed: float) -> bool:
    pass
def StopAll() -> None:
    pass
def StopEffect(handle: int) -> None:
    pass
def ValidateAllEffectPackages() -> bool:
    pass
def ValidateEffectPackages(relativePaths: typing.List[str]) -> bool:
    pass
