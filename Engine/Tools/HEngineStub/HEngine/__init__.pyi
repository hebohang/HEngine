from __future__ import annotations
import HEngine
import typing
from _frozen_importlib import BuiltinImporter as __loader__

__all__ = [
    "BehaviorTreeComponent",
    "Blackboard",
    "BoxCollider2DComponent",
    "CameraComponent",
    "CircleCollider2DComponent",
    "CircleRendererComponent",
    "DirectionalLightComponent",
    "Entity",
    "HE_CORE_ERROR",
    "HE_CORE_INFO",
    "HE_CORE_TRACE",
    "HE_CORE_WARN",
    "HE_ERROR",
    "HE_INFO",
    "HE_TRACE",
    "HE_WARN",
    "Input",
    "KeyCode",
    "MeshComponent",
    "PointLightComponent",
    "PythonScriptComponent",
    "Rigidbody2DComponent",
    "Rigidbody3DComponent",
    "SoundComponent",
    "SpriteRendererComponent",
    "TagComponent",
    "Timestep",
    "TransformComponent",
    "mat4",
    "vec2",
    "vec3",
    "vec4"
]


class BoxCollider2DComponent():
    def __init__(self) -> None: ...
    @property
    def Density(self) -> float:
        """
        :type: float
        """
    @Density.setter
    def Density(self, arg0: float) -> None:
        pass
    @property
    def Friction(self) -> float:
        """
        :type: float
        """
    @Friction.setter
    def Friction(self, arg0: float) -> None:
        pass
    @property
    def Offset(self) -> vec2:
        """
        :type: vec2
        """
    @Offset.setter
    def Offset(self, arg0: vec2) -> None:
        pass
    @property
    def Restitution(self) -> float:
        """
        :type: float
        """
    @Restitution.setter
    def Restitution(self, arg0: float) -> None:
        pass
    @property
    def RestitutionThreshold(self) -> float:
        """
        :type: float
        """
    @RestitutionThreshold.setter
    def RestitutionThreshold(self, arg0: float) -> None:
        pass
    @property
    def Size(self) -> vec2:
        """
        :type: vec2
        """
    @Size.setter
    def Size(self, arg0: vec2) -> None:
        pass
    pass
class Blackboard:
    def set_float(self, key: str, value: float) -> None: ...
    def get_float(self, key: str, default_val: float = 0.0) -> float: ...
    def set_int(self, key: str, value: int) -> None: ...
    def get_int(self, key: str, default_val: int = 0) -> int: ...
    def set_bool(self, key: str, value: bool) -> None: ...
    def get_bool(self, key: str, default_val: bool = False) -> bool: ...
    def set_vec3(self, key: str, value: vec3) -> None: ...
    def get_vec3(self, key: str, default_val: vec3 = ...) -> vec3: ...
    def set_entity_handle(self, key: str, value: int) -> None: ...
    def get_entity_handle(self, key: str, default_val: int = 0) -> int: ...
    def has_key(self, key: str) -> bool: ...
    def remove_key(self, key: str) -> None: ...
    def clear(self) -> None: ...
    def get_all_keys(self) -> list[str]: ...
    pass
class BehaviorTreeComponent():
    def __init__(self) -> None: ...
    def get_blackboard(self) -> Blackboard: ...
    def is_enabled(self) -> bool: ...
    def set_enabled(self, enabled: bool) -> None: ...
    def get_bt_res_path(self) -> str: ...
    def set_bt_res_path(self, path: str) -> None: ...
    @property
    def btResPath(self) -> str:
        """
        :type: str
        """
    @btResPath.setter
    def btResPath(self, arg0: str) -> None:
        pass
    @property
    def tickInterval(self) -> float:
        """
        :type: float
        """
    @tickInterval.setter
    def tickInterval(self, arg0: float) -> None:
        pass
    pass
class CameraComponent():
    def __init__(self) -> None: ...
    @property
    def Camera(self) -> HEngine::SceneCamera:
        """
        :type: HEngine::SceneCamera
        """
    @Camera.setter
    def Camera(self, arg0: HEngine::SceneCamera) -> None:
        pass
    @property
    def FixedAspectRatio(self) -> bool:
        """
        :type: bool
        """
    @FixedAspectRatio.setter
    def FixedAspectRatio(self, arg0: bool) -> None:
        pass
    @property
    def Primary(self) -> bool:
        """
        :type: bool
        """
    @Primary.setter
    def Primary(self, arg0: bool) -> None:
        pass
    pass
class CircleCollider2DComponent():
    def __init__(self) -> None: ...
    @property
    def Density(self) -> float:
        """
        :type: float
        """
    @Density.setter
    def Density(self, arg0: float) -> None:
        pass
    @property
    def Friction(self) -> float:
        """
        :type: float
        """
    @Friction.setter
    def Friction(self, arg0: float) -> None:
        pass
    @property
    def Offset(self) -> vec2:
        """
        :type: vec2
        """
    @Offset.setter
    def Offset(self, arg0: vec2) -> None:
        pass
    @property
    def Radius(self) -> float:
        """
        :type: float
        """
    @Radius.setter
    def Radius(self, arg0: float) -> None:
        pass
    @property
    def Restitution(self) -> float:
        """
        :type: float
        """
    @Restitution.setter
    def Restitution(self, arg0: float) -> None:
        pass
    @property
    def RestitutionThreshold(self) -> float:
        """
        :type: float
        """
    @RestitutionThreshold.setter
    def RestitutionThreshold(self, arg0: float) -> None:
        pass
    pass
class CircleRendererComponent():
    def __init__(self) -> None: ...
    @property
    def Color(self) -> vec4:
        """
        :type: vec4
        """
    @Color.setter
    def Color(self, arg0: vec4) -> None:
        pass
    @property
    def Fade(self) -> float:
        """
        :type: float
        """
    @Fade.setter
    def Fade(self, arg0: float) -> None:
        pass
    @property
    def Radius(self) -> float:
        """
        :type: float
        """
    @Radius.setter
    def Radius(self, arg0: float) -> None:
        pass
    @property
    def Thickness(self) -> float:
        """
        :type: float
        """
    @Thickness.setter
    def Thickness(self, arg0: float) -> None:
        pass
    pass
class DirectionalLightComponent():
    def __init__(self) -> None: ...
    @property
    def Intensity(self) -> float:
        """
        :type: float
        """
    @Intensity.setter
    def Intensity(self, arg0: float) -> None:
        pass
    pass
class Entity():
    def GetComponent(self, arg0: str) -> typing.Union[TransformComponent, PythonScriptComponent, TagComponent, BehaviorTreeComponent]: ...
    def __init__(self) -> None: ...
    pass
class MeshComponent():
    def __init__(self) -> None: ...
    @property
    def Path(self) -> str:
        """
        :type: str
        """
    @Path.setter
    def Path(self, arg0: str) -> None:
        pass
    @property
    def meshResPath(self) -> str:
        """
        :type: str
        """
    @meshResPath.setter
    def meshResPath(self, arg0: str) -> None:
        pass
    pass
class PointLightComponent():
    def __init__(self) -> None: ...
    @property
    def Intensity(self) -> float:
        """
        :type: float
        """
    @Intensity.setter
    def Intensity(self, arg0: float) -> None:
        pass
    @property
    def LightColor(self) -> vec3:
        """
        :type: vec3
        """
    @LightColor.setter
    def LightColor(self, arg0: vec3) -> None:
        pass
    pass
class PythonScriptComponent():
    def __init__(self) -> None: ...
    @property
    def Path(self) -> str:
        """
        :type: str
        """
    @Path.setter
    def Path(self, arg0: str) -> None:
        pass
    pass
class Rigidbody2DComponent():
    def __init__(self) -> None: ...
    @property
    def FixedRotation(self) -> bool:
        """
        :type: bool
        """
    @FixedRotation.setter
    def FixedRotation(self, arg0: bool) -> None:
        pass
    @property
    def Type(self) -> HEngine::Rigidbody2DComponent::BodyType:
        """
        :type: HEngine::Rigidbody2DComponent::BodyType
        """
    @Type.setter
    def Type(self, arg0: HEngine::Rigidbody2DComponent::BodyType) -> None:
        pass
    pass
class Rigidbody3DComponent():
    def __init__(self) -> None: ...
    @property
    def Shape(self) -> HEngine::CollisionShape:
        """
        :type: HEngine::CollisionShape
        """
    @Shape.setter
    def Shape(self, arg0: HEngine::CollisionShape) -> None:
        pass
    @property
    def Type(self) -> HEngine::Rigidbody3DComponent::Body3DType:
        """
        :type: HEngine::Rigidbody3DComponent::Body3DType
        """
    @Type.setter
    def Type(self, arg0: HEngine::Rigidbody3DComponent::Body3DType) -> None:
        pass
    @property
    def angularDamping(self) -> float:
        """
        :type: float
        """
    @angularDamping.setter
    def angularDamping(self, arg0: float) -> None:
        pass
    @property
    def friction(self) -> float:
        """
        :type: float
        """
    @friction.setter
    def friction(self, arg0: float) -> None:
        pass
    @property
    def linearDamping(self) -> float:
        """
        :type: float
        """
    @linearDamping.setter
    def linearDamping(self, arg0: float) -> None:
        pass
    @property
    def mass(self) -> float:
        """
        :type: float
        """
    @mass.setter
    def mass(self, arg0: float) -> None:
        pass
    @property
    def restitution(self) -> float:
        """
        :type: float
        """
    @restitution.setter
    def restitution(self, arg0: float) -> None:
        pass
    pass
class SoundComponent():
    def __init__(self) -> None: ...
    @property
    def Path(self) -> str:
        """
        :type: str
        """
    @Path.setter
    def Path(self, arg0: str) -> None:
        pass
    @property
    def Play(self) -> bool:
        """
        :type: bool
        """
    @Play.setter
    def Play(self, arg0: bool) -> None:
        pass
    pass
class SpriteRendererComponent():
    def __init__(self) -> None: ...
    @property
    def Color(self) -> vec4:
        """
        :type: vec4
        """
    @Color.setter
    def Color(self, arg0: vec4) -> None:
        pass
    @property
    def TilingFactor(self) -> float:
        """
        :type: float
        """
    @TilingFactor.setter
    def TilingFactor(self, arg0: float) -> None:
        pass
    pass
class TagComponent():
    def __init__(self) -> None: ...
    @property
    def Tag(self) -> str:
        """
        :type: str
        """
    @Tag.setter
    def Tag(self, arg0: str) -> None:
        pass
    pass
class Timestep():
    def GetSeconds(self) -> float: ...
    def __init__(self, arg0: float) -> None: ...
    pass
class TransformComponent():
    def __init__(self) -> None: ...
    @property
    def Rotation(self) -> vec3:
        """
        :type: vec3
        """
    @Rotation.setter
    def Rotation(self, arg0: vec3) -> None:
        pass
    @property
    def Scale(self) -> vec3:
        """
        :type: vec3
        """
    @Scale.setter
    def Scale(self, arg0: vec3) -> None:
        pass
    @property
    def Translation(self) -> vec3:
        """
        :type: vec3
        """
    @Translation.setter
    def Translation(self, arg0: vec3) -> None:
        pass
    pass
class mat4():
    def __init__(self, arg0: float) -> None: ...
    pass
class vec2():
    def __init__(self, arg0: float, arg1: float) -> None: ...
    @property
    def x(self) -> float:
        """
        :type: float
        """
    @x.setter
    def x(self, arg0: float) -> None:
        pass
    @property
    def y(self) -> float:
        """
        :type: float
        """
    @y.setter
    def y(self, arg0: float) -> None:
        pass
    pass
class vec3():
    def __init__(self, arg0: float, arg1: float, arg2: float) -> None: ...
    @property
    def x(self) -> float:
        """
        :type: float
        """
    @x.setter
    def x(self, arg0: float) -> None:
        pass
    @property
    def y(self) -> float:
        """
        :type: float
        """
    @y.setter
    def y(self, arg0: float) -> None:
        pass
    @property
    def z(self) -> float:
        """
        :type: float
        """
    @z.setter
    def z(self, arg0: float) -> None:
        pass
    pass
class vec4():
    def __init__(self, arg0: float, arg1: float, arg2: float, arg3: float) -> None: ...
    @property
    def w(self) -> float:
        """
        :type: float
        """
    @w.setter
    def w(self, arg0: float) -> None:
        pass
    @property
    def x(self) -> float:
        """
        :type: float
        """
    @x.setter
    def x(self, arg0: float) -> None:
        pass
    @property
    def y(self) -> float:
        """
        :type: float
        """
    @y.setter
    def y(self, arg0: float) -> None:
        pass
    @property
    def z(self) -> float:
        """
        :type: float
        """
    @z.setter
    def z(self, arg0: float) -> None:
        pass
    pass
def HE_CORE_ERROR(arg0: str) -> None:
    pass
def HE_CORE_INFO(arg0: str) -> None:
    pass
def HE_CORE_TRACE(arg0: str) -> None:
    pass
def HE_CORE_WARN(arg0: str) -> None:
    pass
def HE_ERROR(arg0: str) -> None:
    pass
def HE_INFO(arg0: str) -> None:
    pass
def HE_TRACE(arg0: str) -> None:
    pass
def HE_WARN(arg0: str) -> None:
    pass
