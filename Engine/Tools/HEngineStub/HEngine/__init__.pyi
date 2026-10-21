from __future__ import annotations
import HEngine
import typing
from . import Audio as Audio
from . import GM as GM
from . import GamepadAxis as GamepadAxis
from . import GamepadButton as GamepadButton
from . import Input as Input
from . import KeyCode as KeyCode
from . import MouseCode as MouseCode
from . import RmlUI as RmlUI
from . import RuntimeResourceWarmup as RuntimeResourceWarmup
from . import Script as Script
from . import Settings as Settings
from . import UI as UI
from . import VFX as VFX
from _frozen_importlib import BuiltinImporter as __loader__

__all__ = [
    "AnimatorComponent",
    "ApplySurvivorAgentHit",
    "ApplySurvivorAgentStatus",
    "Audio",
    "AudioListenerComponent",
    "BehaviorTreeComponent",
    "Blackboard",
    "BoxCollider2DComponent",
    "CameraComponent",
    "CapsuleCollider3DComponent",
    "CharacterMovementComponent",
    "CircleCollider2DComponent",
    "CircleRendererComponent",
    "CollisionShape",
    "DeactivateSurvivorAgent",
    "DeactivateSurvivorPickup",
    "Destroy",
    "DirectionalLightComponent",
    "Entity",
    "EnvironmentLightComponent",
    "FindEntityByTag",
    "FindInstanceEntity",
    "FindSurvivorAgentCircleOverlap",
    "FollowCameraComponent",
    "FollowCameraMode",
    "GM",
    "GamepadAxis",
    "GamepadButton",
    "GamepadState",
    "GetFullPath",
    "GetSavedPath",
    "HE_CORE_ERROR",
    "HE_CORE_INFO",
    "HE_CORE_TRACE",
    "HE_CORE_WARN",
    "HE_ERROR",
    "HE_INFO",
    "HE_TRACE",
    "HE_WARN",
    "Input",
    "Instantiate",
    "KeyCode",
    "KillSurvivorAgent",
    "MeshComponent",
    "MouseCode",
    "NativeScriptComponent",
    "PointLightComponent",
    "PostProcessingComponent",
    "PrefabInstanceComponent",
    "PythonScriptComponent",
    "QuerySurvivorAgentCorridors",
    "QuerySurvivorAgentsInRadius",
    "QuerySurvivorAgentsInSweptAnnulus",
    "QuerySurvivorAgentsInSweptCapsule",
    "RaycastFloorHeight",
    "ReflectionProbeComponent",
    "RegisterSurvivorAgent",
    "RegisterSurvivorPickup",
    "RenderSettingsComponent",
    "RequestRuntimeScene",
    "ResetSurvivorAgents",
    "ResetSurvivorPickups",
    "Rigidbody2DComponent",
    "Rigidbody3DComponent",
    "RmlUI",
    "RuntimeResourceWarmup",
    "SceneCamera",
    "Script",
    "ScriptComponent",
    "SelectDensestSurvivorAgents",
    "SelectSurvivorAgentChain",
    "SetRuntimePaused",
    "SetSurvivorAgentDistanceRetirement",
    "SetSurvivorAgentHealth",
    "SetSurvivorPickupScaleBonus",
    "Settings",
    "SoundComponent",
    "SpotLightComponent",
    "SpringArmComponent",
    "SpriteRendererComponent",
    "TagComponent",
    "Timestep",
    "TransformComponent",
    "UI",
    "UUID",
    "UpdateSurvivorAgents",
    "UpdateSurvivorCrowd",
    "UpdateSurvivorPickups",
    "VFX",
    "mat4",
    "vec2",
    "vec3",
    "vec4"
]


class AnimatorComponent():
    def ConsumeEvents(self) -> object: 
        """
        (self: HEngine.AnimatorComponent) -> object
        """
    def GetClipCount(self) -> int: 
        """
        (self: HEngine.AnimatorComponent) -> int
        """
    def GetClipName(self, index: int) -> str: 
        """
        (self: HEngine.AnimatorComponent, index: int) -> str
        """
    def GetControllerPath(self) -> str: 
        """
        (self: HEngine.AnimatorComponent) -> str
        """
    def GetProgress(self) -> float: 
        """
        (self: HEngine.AnimatorComponent) -> float
        """
    def Pause(self) -> None: 
        """
        (self: HEngine.AnimatorComponent) -> None
        """
    def Play(self, clipIndex: int = -1) -> None: 
        """
        (self: HEngine.AnimatorComponent, clipIndex: int = -1) -> None
        """
    def Resume(self) -> None: 
        """
        (self: HEngine.AnimatorComponent) -> None
        """
    def SetBool(self, name: str, value: bool) -> None: 
        """
        (self: HEngine.AnimatorComponent, name: str, value: bool) -> None
        """
    def SetControllerPath(self, path: str) -> None: 
        """
        (self: HEngine.AnimatorComponent, path: str) -> None
        """
    def SetSpeed(self, speed: float) -> None: 
        """
        (self: HEngine.AnimatorComponent, speed: float) -> None
        """
    def SetTrigger(self, name: str) -> None: 
        """
        (self: HEngine.AnimatorComponent, name: str) -> None
        """
    def Step(self, seconds: float) -> None: 
        """
        (self: HEngine.AnimatorComponent, seconds: float) -> None
        """
    def Stop(self) -> None: 
        """
        (self: HEngine.AnimatorComponent) -> None
        """
    def __init__(self) -> None: ...
    def get_bool(self, name: str, default_value: bool = False) -> bool: 
        """
        (self: HEngine.AnimatorComponent, name: str, default_value: bool = False) -> bool
        """
    def get_controller_path(self) -> str: 
        """
        (self: HEngine.AnimatorComponent) -> str
        """
    def get_current_state(self) -> str: 
        """
        (self: HEngine.AnimatorComponent) -> str
        """
    def get_float(self, name: str, default_value: float = 0.0) -> float: 
        """
        (self: HEngine.AnimatorComponent, name: str, default_value: float = 0.0) -> float
        """
    def get_int(self, name: str, default_value: int = 0) -> int: 
        """
        (self: HEngine.AnimatorComponent, name: str, default_value: int = 0) -> int
        """
    def get_trigger(self, name: str) -> bool: 
        """
        (self: HEngine.AnimatorComponent, name: str) -> bool
        """
    def has_parameter(self, name: str) -> bool: 
        """
        (self: HEngine.AnimatorComponent, name: str) -> bool
        """
    def reset_trigger(self, name: str) -> None: 
        """
        (self: HEngine.AnimatorComponent, name: str) -> None
        """
    def set_bool(self, name: str, value: bool) -> None: 
        """
        (self: HEngine.AnimatorComponent, name: str, value: bool) -> None
        """
    def set_controller_path(self, path: str) -> None: 
        """
        (self: HEngine.AnimatorComponent, path: str) -> None
        """
    def set_float(self, name: str, value: float) -> None: 
        """
        (self: HEngine.AnimatorComponent, name: str, value: float) -> None
        """
    def set_int(self, name: str, value: int) -> None: 
        """
        (self: HEngine.AnimatorComponent, name: str, value: int) -> None
        """
    def set_trigger(self, name: str) -> None: 
        """
        (self: HEngine.AnimatorComponent, name: str) -> None
        """
    @property
    def animResPath(self) -> str:
        """
        :type: str
        """
    @animResPath.setter
    def animResPath(self, arg0: str) -> None:
        pass
    @property
    def bEnableRootMotion(self) -> bool:
        """
        :type: bool
        """
    @bEnableRootMotion.setter
    def bEnableRootMotion(self, arg0: bool) -> None:
        pass
    @property
    def bLockRootMotionXZ(self) -> bool:
        """
        :type: bool
        """
    @bLockRootMotionXZ.setter
    def bLockRootMotionXZ(self, arg0: bool) -> None:
        pass
    @property
    def bLoop(self) -> bool:
        """
        :type: bool
        """
    @bLoop.setter
    def bLoop(self, arg0: bool) -> None:
        pass
    @property
    def bPlay(self) -> bool:
        """
        :type: bool
        """
    @bPlay.setter
    def bPlay(self, arg0: bool) -> None:
        pass
    @property
    def controllerPath(self) -> str:
        """
        :type: str
        """
    @controllerPath.setter
    def controllerPath(self, arg0: str) -> None:
        pass
    @property
    def currentClipIndex(self) -> int:
        """
        :type: int
        """
    @currentClipIndex.setter
    def currentClipIndex(self, arg0: int) -> None:
        pass
    @property
    def playSpeed(self) -> float:
        """
        :type: float
        """
    @playSpeed.setter
    def playSpeed(self, arg0: float) -> None:
        pass
    pass
class AudioListenerComponent():
    def __init__(self) -> None: ...
    @property
    def bActive(self) -> bool:
        """
        :type: bool
        """
    @bActive.setter
    def bActive(self, arg0: bool) -> None:
        pass
    @property
    def bUsePrimaryCameraOrientation(self) -> bool:
        """
        :type: bool
        """
    @bUsePrimaryCameraOrientation.setter
    def bUsePrimaryCameraOrientation(self, arg0: bool) -> None:
        pass
    pass
class BehaviorTreeComponent():
    def __init__(self) -> None: ...
    def get_blackboard(self) -> Blackboard: 
        """
        (self: HEngine.BehaviorTreeComponent) -> HEngine.Blackboard
        """
    def get_bt_res_path(self) -> str: 
        """
        (self: HEngine.BehaviorTreeComponent) -> str
        """
    def is_enabled(self) -> bool: 
        """
        (self: HEngine.BehaviorTreeComponent) -> bool
        """
    def set_bt_res_path(self, path: str) -> None: 
        """
        (self: HEngine.BehaviorTreeComponent, path: str) -> None
        """
    def set_enabled(self, enabled: bool) -> None: 
        """
        (self: HEngine.BehaviorTreeComponent, enabled: bool) -> None
        """
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
class Blackboard():
    def clear(self) -> None: ...
    def get_all_keys(self) -> typing.List[str]: ...
    def get_bool(self, key: str, default_val: bool = False) -> bool: ...
    def get_entity_handle(self, key: str, default_val: int = 4294967295) -> int: ...
    def get_float(self, key: str, default_val: float = 0.0) -> float: ...
    def get_int(self, key: str, default_val: int = 0) -> int: ...
    def get_vec3(self, key: str, default_val: vec3 = ...) -> vec3: ...
    def has_key(self, key: str) -> bool: ...
    def remove_key(self, key: str) -> None: ...
    def set_bool(self, key: str, value: bool) -> None: ...
    def set_entity_handle(self, key: str, value: int) -> None: ...
    def set_float(self, key: str, value: float) -> None: ...
    def set_int(self, key: str, value: int) -> None: ...
    def set_vec3(self, key: str, value: vec3) -> None: ...
    pass
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
class CameraComponent():
    def __init__(self) -> None: ...
    @property
    def Camera(self) -> SceneCamera:
        """
        :type: SceneCamera
        """
    @Camera.setter
    def Camera(self, arg0: SceneCamera) -> None:
        pass
    @property
    def ExposureCompensation(self) -> float:
        """
        :type: float
        """
    @ExposureCompensation.setter
    def ExposureCompensation(self, arg0: float) -> None:
        pass
    @property
    def ExposureEV100(self) -> float:
        """
        :type: float
        """
    @ExposureEV100.setter
    def ExposureEV100(self, arg0: float) -> None:
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
    @property
    def bOverrideExposure(self) -> bool:
        """
        :type: bool
        """
    @bOverrideExposure.setter
    def bOverrideExposure(self, arg0: bool) -> None:
        pass
    pass
class CapsuleCollider3DComponent():
    def __init__(self) -> None: ...
    @property
    def Center(self) -> vec3:
        """
        :type: vec3
        """
    @Center.setter
    def Center(self, arg0: vec3) -> None:
        pass
    @property
    def Height(self) -> float:
        """
        :type: float
        """
    @Height.setter
    def Height(self, arg0: float) -> None:
        pass
    @property
    def IsTrigger(self) -> bool:
        """
        :type: bool
        """
    @IsTrigger.setter
    def IsTrigger(self, arg0: bool) -> None:
        pass
    @property
    def Radius(self) -> float:
        """
        :type: float
        """
    @Radius.setter
    def Radius(self, arg0: float) -> None:
        pass
    pass
class CharacterMovementComponent():
    def __init__(self) -> None: ...
    @property
    def Acceleration(self) -> float:
        """
        :type: float
        """
    @Acceleration.setter
    def Acceleration(self, arg0: float) -> None:
        pass
    @property
    def Deceleration(self) -> float:
        """
        :type: float
        """
    @Deceleration.setter
    def Deceleration(self, arg0: float) -> None:
        pass
    @property
    def Gravity(self) -> float:
        """
        :type: float
        """
    @Gravity.setter
    def Gravity(self, arg0: float) -> None:
        pass
    @property
    def JumpSpeed(self) -> float:
        """
        :type: float
        """
    @JumpSpeed.setter
    def JumpSpeed(self, arg0: float) -> None:
        pass
    @property
    def MaxSlopeAngleDegrees(self) -> float:
        """
        :type: float
        """
    @MaxSlopeAngleDegrees.setter
    def MaxSlopeAngleDegrees(self, arg0: float) -> None:
        pass
    @property
    def MaxSpeed(self) -> float:
        """
        :type: float
        """
    @MaxSpeed.setter
    def MaxSpeed(self, arg0: float) -> None:
        pass
    @property
    def SprintAccelerationMultiplier(self) -> float:
        """
        :type: float
        """
    @SprintAccelerationMultiplier.setter
    def SprintAccelerationMultiplier(self, arg0: float) -> None:
        pass
    @property
    def SprintSpeedMultiplier(self) -> float:
        """
        :type: float
        """
    @SprintSpeedMultiplier.setter
    def SprintSpeedMultiplier(self, arg0: float) -> None:
        pass
    @property
    def StepUpHeight(self) -> float:
        """
        :type: float
        """
    @StepUpHeight.setter
    def StepUpHeight(self, arg0: float) -> None:
        pass
    @property
    def StickToFloorDistance(self) -> float:
        """
        :type: float
        """
    @StickToFloorDistance.setter
    def StickToFloorDistance(self, arg0: float) -> None:
        pass
    @property
    def TurnRate(self) -> float:
        """
        :type: float
        """
    @TurnRate.setter
    def TurnRate(self, arg0: float) -> None:
        pass
    @property
    def bEnableJump(self) -> bool:
        """
        :type: bool
        """
    @bEnableJump.setter
    def bEnableJump(self, arg0: bool) -> None:
        pass
    @property
    def bEnableSprint(self) -> bool:
        """
        :type: bool
        """
    @bEnableSprint.setter
    def bEnableSprint(self, arg0: bool) -> None:
        pass
    @property
    def bOrientRotationToMovement(self) -> bool:
        """
        :type: bool
        """
    @bOrientRotationToMovement.setter
    def bOrientRotationToMovement(self, arg0: bool) -> None:
        pass
    @property
    def bPlayerInputEnabled(self) -> bool:
        """
        :type: bool
        """
    @bPlayerInputEnabled.setter
    def bPlayerInputEnabled(self, arg0: bool) -> None:
        pass
    @property
    def bUseWorldUp(self) -> bool:
        """
        :type: bool
        """
    @bUseWorldUp.setter
    def bUseWorldUp(self, arg0: bool) -> None:
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
class CollisionShape():
    """
    Members:

      None_

      Box

      Sphere

      ConvexHull

      TriangleMesh
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
    Box: HEngine.CollisionShape # value = <CollisionShape.Box: 1>
    ConvexHull: HEngine.CollisionShape # value = <CollisionShape.ConvexHull: 3>
    None_: HEngine.CollisionShape # value = <CollisionShape.None_: 0>
    Sphere: HEngine.CollisionShape # value = <CollisionShape.Sphere: 2>
    TriangleMesh: HEngine.CollisionShape # value = <CollisionShape.TriangleMesh: 4>
    __members__: dict # value = {'None_': <CollisionShape.None_: 0>, 'Box': <CollisionShape.Box: 1>, 'Sphere': <CollisionShape.Sphere: 2>, 'ConvexHull': <CollisionShape.ConvexHull: 3>, 'TriangleMesh': <CollisionShape.TriangleMesh: 4>}
    pass
class DirectionalLightComponent():
    def __init__(self) -> None: ...
    @property
    def CascadeCount(self) -> int:
        """
        :type: int
        """
    @CascadeCount.setter
    def CascadeCount(self, arg0: int) -> None:
        pass
    @property
    def CascadeLambda(self) -> float:
        """
        :type: float
        """
    @CascadeLambda.setter
    def CascadeLambda(self, arg0: float) -> None:
        pass
    @property
    def Color(self) -> vec3:
        """
        :type: vec3
        """
    @Color.setter
    def Color(self, arg0: vec3) -> None:
        pass
    @property
    def DepthBias(self) -> float:
        """
        :type: float
        """
    @DepthBias.setter
    def DepthBias(self, arg0: float) -> None:
        pass
    @property
    def Intensity(self) -> float:
        """
        :type: float
        """
    @Intensity.setter
    def Intensity(self, arg0: float) -> None:
        pass
    @property
    def NormalBias(self) -> float:
        """
        :type: float
        """
    @NormalBias.setter
    def NormalBias(self, arg0: float) -> None:
        pass
    @property
    def PCFRadius(self) -> float:
        """
        :type: float
        """
    @PCFRadius.setter
    def PCFRadius(self, arg0: float) -> None:
        pass
    @property
    def ShadowDistance(self) -> float:
        """
        :type: float
        """
    @ShadowDistance.setter
    def ShadowDistance(self, arg0: float) -> None:
        pass
    @property
    def ShadowMapSize(self) -> int:
        """
        :type: int
        """
    @ShadowMapSize.setter
    def ShadowMapSize(self, arg0: int) -> None:
        pass
    @property
    def ShadowStrength(self) -> float:
        """
        :type: float
        """
    @ShadowStrength.setter
    def ShadowStrength(self, arg0: float) -> None:
        pass
    @property
    def bCastShadows(self) -> bool:
        """
        :type: bool
        """
    @bCastShadows.setter
    def bCastShadows(self, arg0: bool) -> None:
        pass
    @property
    def bUsePCF(self) -> bool:
        """
        :type: bool
        """
    @bUsePCF.setter
    def bUsePCF(self, arg0: bool) -> None:
        pass
    pass
class Entity():
    def GetComponent(self, arg0: str) -> object: ...
    def GetUUID(self) -> str: ...
    def GetWorldPosition(self) -> vec3: ...
    def HasComponent(self, arg0: str) -> bool: ...
    @typing.overload
    def Move(self, arg0: float, arg1: float, arg2: float) -> None: ...
    @typing.overload
    def Move(self, arg0: vec3) -> None: ...
    def SetMovementInputEnabled(self, arg0: bool) -> None: ...
    def SetVisual(self, mesh_path: str, animation_controller_path: str, uniform_scale: float) -> None: ...
    def SetVisualSilhouette(self, enabled: bool) -> None: ...
    def Teleport(self, arg0: vec3) -> bool: ...
    def __init__(self) -> None: ...
    pass
class EnvironmentLightComponent():
    def __init__(self) -> None: ...
    @property
    def CloudAmbient(self) -> float:
        """
        :type: float
        """
    @CloudAmbient.setter
    def CloudAmbient(self, arg0: float) -> None:
        pass
    @property
    def CloudBottom(self) -> float:
        """
        :type: float
        """
    @CloudBottom.setter
    def CloudBottom(self, arg0: float) -> None:
        pass
    @property
    def CloudCoverage(self) -> float:
        """
        :type: float
        """
    @CloudCoverage.setter
    def CloudCoverage(self, arg0: float) -> None:
        pass
    @property
    def CloudDensity(self) -> float:
        """
        :type: float
        """
    @CloudDensity.setter
    def CloudDensity(self, arg0: float) -> None:
        pass
    @property
    def CloudErosion(self) -> float:
        """
        :type: float
        """
    @CloudErosion.setter
    def CloudErosion(self, arg0: float) -> None:
        pass
    @property
    def CloudEvolutionSpeed(self) -> float:
        """
        :type: float
        """
    @CloudEvolutionSpeed.setter
    def CloudEvolutionSpeed(self, arg0: float) -> None:
        pass
    @property
    def CloudLitColor(self) -> vec3:
        """
        :type: vec3
        """
    @CloudLitColor.setter
    def CloudLitColor(self, arg0: vec3) -> None:
        pass
    @property
    def CloudMaxDistance(self) -> float:
        """
        :type: float
        """
    @CloudMaxDistance.setter
    def CloudMaxDistance(self, arg0: float) -> None:
        pass
    @property
    def CloudRoundness(self) -> float:
        """
        :type: float
        """
    @CloudRoundness.setter
    def CloudRoundness(self, arg0: float) -> None:
        pass
    @property
    def CloudScale(self) -> float:
        """
        :type: float
        """
    @CloudScale.setter
    def CloudScale(self, arg0: float) -> None:
        pass
    @property
    def CloudSeed(self) -> int:
        """
        :type: int
        """
    @CloudSeed.setter
    def CloudSeed(self, arg0: int) -> None:
        pass
    @property
    def CloudShadowColor(self) -> vec3:
        """
        :type: vec3
        """
    @CloudShadowColor.setter
    def CloudShadowColor(self, arg0: vec3) -> None:
        pass
    @property
    def CloudSilverLining(self) -> float:
        """
        :type: float
        """
    @CloudSilverLining.setter
    def CloudSilverLining(self, arg0: float) -> None:
        pass
    @property
    def CloudThickness(self) -> float:
        """
        :type: float
        """
    @CloudThickness.setter
    def CloudThickness(self, arg0: float) -> None:
        pass
    @property
    def CloudWindAngle(self) -> float:
        """
        :type: float
        """
    @CloudWindAngle.setter
    def CloudWindAngle(self, arg0: float) -> None:
        pass
    @property
    def CloudWindSpeed(self) -> float:
        """
        :type: float
        """
    @CloudWindSpeed.setter
    def CloudWindSpeed(self, arg0: float) -> None:
        pass
    @property
    def ExposureCompensation(self) -> float:
        """
        :type: float
        """
    @ExposureCompensation.setter
    def ExposureCompensation(self, arg0: float) -> None:
        pass
    @property
    def ExposureEV100(self) -> float:
        """
        :type: float
        """
    @ExposureEV100.setter
    def ExposureEV100(self, arg0: float) -> None:
        pass
    @property
    def HdrTexturePath(self) -> str:
        """
        :type: str
        """
    @HdrTexturePath.setter
    def HdrTexturePath(self, arg0: str) -> None:
        pass
    @property
    def IBLIntensity(self) -> float:
        """
        :type: float
        """
    @IBLIntensity.setter
    def IBLIntensity(self, arg0: float) -> None:
        pass
    @property
    def bCloudsEnabled(self) -> bool:
        """
        :type: bool
        """
    @bCloudsEnabled.setter
    def bCloudsEnabled(self, arg0: bool) -> None:
        pass
    pass
class FollowCameraComponent():
    def __init__(self) -> None: ...
    @property
    def InitialPitchDegrees(self) -> float:
        """
        :type: float
        """
    @InitialPitchDegrees.setter
    def InitialPitchDegrees(self, arg0: float) -> None:
        pass
    @property
    def MaxPitchDegrees(self) -> float:
        """
        :type: float
        """
    @MaxPitchDegrees.setter
    def MaxPitchDegrees(self, arg0: float) -> None:
        pass
    @property
    def MinPitchDegrees(self) -> float:
        """
        :type: float
        """
    @MinPitchDegrees.setter
    def MinPitchDegrees(self, arg0: float) -> None:
        pass
    @property
    def Mode(self) -> FollowCameraMode:
        """
        :type: FollowCameraMode
        """
    @Mode.setter
    def Mode(self, arg0: FollowCameraMode) -> None:
        pass
    @property
    def MouseSensitivity(self) -> float:
        """
        :type: float
        """
    @MouseSensitivity.setter
    def MouseSensitivity(self, arg0: float) -> None:
        pass
    @property
    def TargetUUID(self) -> UUID:
        """
        (self: HEngine.FollowCameraComponent) -> HEngine.UUID

        :type: UUID
        """
    @TargetUUID.setter
    def TargetUUID(self, arg0: UUID) -> None:
        """
        (self: HEngine.FollowCameraComponent) -> HEngine.UUID
        """
    @property
    def TopDownDistance(self) -> float:
        """
        :type: float
        """
    @TopDownDistance.setter
    def TopDownDistance(self, arg0: float) -> None:
        pass
    @property
    def TopDownPitchDegrees(self) -> float:
        """
        :type: float
        """
    @TopDownPitchDegrees.setter
    def TopDownPitchDegrees(self, arg0: float) -> None:
        pass
    @property
    def TopDownYawDegrees(self) -> float:
        """
        :type: float
        """
    @TopDownYawDegrees.setter
    def TopDownYawDegrees(self, arg0: float) -> None:
        pass
    @property
    def bAllowPitchInput(self) -> bool:
        """
        :type: bool
        """
    @bAllowPitchInput.setter
    def bAllowPitchInput(self, arg0: bool) -> None:
        pass
    @property
    def bAllowYawInput(self) -> bool:
        """
        :type: bool
        """
    @bAllowYawInput.setter
    def bAllowYawInput(self, arg0: bool) -> None:
        pass
    @property
    def bFollowTargetRotation(self) -> bool:
        """
        :type: bool
        """
    @bFollowTargetRotation.setter
    def bFollowTargetRotation(self, arg0: bool) -> None:
        pass
    @property
    def bHasTarget(self) -> bool:
        """
        :type: bool
        """
    @bHasTarget.setter
    def bHasTarget(self, arg0: bool) -> None:
        pass
    @property
    def bLockToWorldUp(self) -> bool:
        """
        :type: bool
        """
    @bLockToWorldUp.setter
    def bLockToWorldUp(self, arg0: bool) -> None:
        pass
    @property
    def bRequireHoldToRotate(self) -> bool:
        """
        :type: bool
        """
    @bRequireHoldToRotate.setter
    def bRequireHoldToRotate(self, arg0: bool) -> None:
        pass
    pass
class FollowCameraMode():
    """
    Members:

      ThirdPerson

      TopDown
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
    ThirdPerson: HEngine.FollowCameraMode # value = <FollowCameraMode.ThirdPerson: 0>
    TopDown: HEngine.FollowCameraMode # value = <FollowCameraMode.TopDown: 1>
    __members__: dict # value = {'ThirdPerson': <FollowCameraMode.ThirdPerson: 0>, 'TopDown': <FollowCameraMode.TopDown: 1>}
    pass
class GamepadState():
    def GetAxis(self, arg0: GamepadAxis.GamepadAxis) -> float: ...
    def IsButtonPressed(self, arg0: GamepadButton.GamepadButton) -> bool: ...
    @property
    def Connected(self) -> bool:
        """
        :type: bool
        """
    pass
class MeshComponent():
    def __init__(self) -> None: ...
    @property
    def MaterialOverrides(self) -> typing.List[str]:
        """
        :type: typing.List[str]
        """
    @MaterialOverrides.setter
    def MaterialOverrides(self, arg0: typing.List[str]) -> None:
        pass
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
class NativeScriptComponent():
    def __init__(self) -> None: ...
    @property
    def Type(self) -> str:
        """
        :type: str
        """
    @Type.setter
    def Type(self, arg0: str) -> None:
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
    def IntensityUnits(self) -> int:
        """
        :type: int
        """
    @IntensityUnits.setter
    def IntensityUnits(self, arg0: int) -> None:
        pass
    @property
    def LightColor(self) -> vec3:
        """
        :type: vec3
        """
    @LightColor.setter
    def LightColor(self, arg0: vec3) -> None:
        pass
    @property
    def Radius(self) -> float:
        """
        :type: float
        """
    @Radius.setter
    def Radius(self, arg0: float) -> None:
        pass
    pass
class PostProcessingComponent():
    def __init__(self) -> None: ...
    @property
    def BlendRadius(self) -> float:
        """
        :type: float
        """
    @BlendRadius.setter
    def BlendRadius(self, arg0: float) -> None:
        pass
    @property
    def BlendWeight(self) -> float:
        """
        :type: float
        """
    @BlendWeight.setter
    def BlendWeight(self, arg0: float) -> None:
        pass
    @property
    def BloomIntensity(self) -> float:
        """
        :type: float
        """
    @BloomIntensity.setter
    def BloomIntensity(self, arg0: float) -> None:
        pass
    @property
    def Contrast(self) -> float:
        """
        :type: float
        """
    @Contrast.setter
    def Contrast(self, arg0: float) -> None:
        pass
    @property
    def ExposureCompensation(self) -> float:
        """
        :type: float
        """
    @ExposureCompensation.setter
    def ExposureCompensation(self, arg0: float) -> None:
        pass
    @property
    def ExposureEV100(self) -> float:
        """
        :type: float
        """
    @ExposureEV100.setter
    def ExposureEV100(self, arg0: float) -> None:
        pass
    @property
    def Priority(self) -> float:
        """
        :type: float
        """
    @Priority.setter
    def Priority(self, arg0: float) -> None:
        pass
    @property
    def Saturation(self) -> float:
        """
        :type: float
        """
    @Saturation.setter
    def Saturation(self, arg0: float) -> None:
        pass
    @property
    def bEnabled(self) -> bool:
        """
        :type: bool
        """
    @bEnabled.setter
    def bEnabled(self, arg0: bool) -> None:
        pass
    @property
    def bGlobal(self) -> bool:
        """
        :type: bool
        """
    @bGlobal.setter
    def bGlobal(self, arg0: bool) -> None:
        pass
    @property
    def bLocalVolumeEnabled(self) -> bool:
        """
        :type: bool
        """
    @bLocalVolumeEnabled.setter
    def bLocalVolumeEnabled(self, arg0: bool) -> None:
        pass
    @property
    def bOverrideBloomIntensity(self) -> bool:
        """
        :type: bool
        """
    @bOverrideBloomIntensity.setter
    def bOverrideBloomIntensity(self, arg0: bool) -> None:
        pass
    @property
    def bOverrideContrast(self) -> bool:
        """
        :type: bool
        """
    @bOverrideContrast.setter
    def bOverrideContrast(self, arg0: bool) -> None:
        pass
    @property
    def bOverrideExposure(self) -> bool:
        """
        :type: bool
        """
    @bOverrideExposure.setter
    def bOverrideExposure(self, arg0: bool) -> None:
        pass
    @property
    def bOverrideSaturation(self) -> bool:
        """
        :type: bool
        """
    @bOverrideSaturation.setter
    def bOverrideSaturation(self, arg0: bool) -> None:
        pass
    pass
class PrefabInstanceComponent():
    def __init__(self) -> None: ...
    @property
    def BaselineJson(self) -> str:
        """
        :type: str
        """
    @BaselineJson.setter
    def BaselineJson(self, arg0: str) -> None:
        pass
    @property
    def InstanceRootUuid(self) -> str:
        """
        :type: str
        """
    @InstanceRootUuid.setter
    def InstanceRootUuid(self, arg0: str) -> None:
        pass
    @property
    def LocalId(self) -> str:
        """
        :type: str
        """
    @LocalId.setter
    def LocalId(self, arg0: str) -> None:
        pass
    @property
    def OverridePathsJson(self) -> str:
        """
        :type: str
        """
    @OverridePathsJson.setter
    def OverridePathsJson(self, arg0: str) -> None:
        pass
    @property
    def TemplateDocumentJson(self) -> str:
        """
        :type: str
        """
    @TemplateDocumentJson.setter
    def TemplateDocumentJson(self, arg0: str) -> None:
        pass
    @property
    def TemplatePath(self) -> str:
        """
        :type: str
        """
    @TemplatePath.setter
    def TemplatePath(self, arg0: str) -> None:
        pass
    @property
    def TemplateRevision(self) -> str:
        """
        :type: str
        """
    @TemplateRevision.setter
    def TemplateRevision(self, arg0: str) -> None:
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
class ReflectionProbeComponent():
    def __init__(self) -> None: ...
    @property
    def BlendDistance(self) -> float:
        """
        :type: float
        """
    @BlendDistance.setter
    def BlendDistance(self, arg0: float) -> None:
        pass
    @property
    def BoxHalfExtents(self) -> vec3:
        """
        :type: vec3
        """
    @BoxHalfExtents.setter
    def BoxHalfExtents(self, arg0: vec3) -> None:
        pass
    @property
    def CaptureOffset(self) -> vec3:
        """
        :type: vec3
        """
    @CaptureOffset.setter
    def CaptureOffset(self, arg0: vec3) -> None:
        pass
    @property
    def HdrTexturePath(self) -> str:
        """
        :type: str
        """
    @HdrTexturePath.setter
    def HdrTexturePath(self, arg0: str) -> None:
        pass
    @property
    def Intensity(self) -> float:
        """
        :type: float
        """
    @Intensity.setter
    def Intensity(self, arg0: float) -> None:
        pass
    @property
    def bEnabled(self) -> bool:
        """
        :type: bool
        """
    @bEnabled.setter
    def bEnabled(self, arg0: bool) -> None:
        pass
    pass
class RenderSettingsComponent():
    def __init__(self) -> None: ...
    pass
class Rigidbody2DComponent():
    class BodyType():
        """
        Members:

          Static

          Dynamic

          Kinematic
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
        Dynamic: HEngine.Rigidbody2DComponent.BodyType # value = <BodyType.Dynamic: 1>
        Kinematic: HEngine.Rigidbody2DComponent.BodyType # value = <BodyType.Kinematic: 2>
        Static: HEngine.Rigidbody2DComponent.BodyType # value = <BodyType.Static: 0>
        __members__: dict # value = {'Static': <BodyType.Static: 0>, 'Dynamic': <BodyType.Dynamic: 1>, 'Kinematic': <BodyType.Kinematic: 2>}
        pass
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
    def Type(self) -> HEngine.Rigidbody2DComponent.BodyType:
        """
        :type: HEngine.Rigidbody2DComponent.BodyType
        """
    @Type.setter
    def Type(self, arg0: HEngine.Rigidbody2DComponent.BodyType) -> None:
        pass
    pass
class Rigidbody3DComponent():
    class Body3DType():
        """
        Members:

          Static

          Dynamic

          Kinematic
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
        Dynamic: HEngine.Rigidbody3DComponent.Body3DType # value = <Body3DType.Dynamic: 1>
        Kinematic: HEngine.Rigidbody3DComponent.Body3DType # value = <Body3DType.Kinematic: 2>
        Static: HEngine.Rigidbody3DComponent.Body3DType # value = <Body3DType.Static: 0>
        __members__: dict # value = {'Static': <Body3DType.Static: 0>, 'Dynamic': <Body3DType.Dynamic: 1>, 'Kinematic': <Body3DType.Kinematic: 2>}
        pass
    def __init__(self) -> None: ...
    @property
    def AllowSleeping(self) -> bool:
        """
        :type: bool
        """
    @AllowSleeping.setter
    def AllowSleeping(self, arg0: bool) -> None:
        pass
    @property
    def ColliderOffset(self) -> vec3:
        """
        :type: vec3
        """
    @ColliderOffset.setter
    def ColliderOffset(self, arg0: vec3) -> None:
        pass
    @property
    def ColliderRotation(self) -> vec3:
        """
        :type: vec3
        """
    @ColliderRotation.setter
    def ColliderRotation(self, arg0: vec3) -> None:
        pass
    @property
    def ColliderScale(self) -> vec3:
        """
        :type: vec3
        """
    @ColliderScale.setter
    def ColliderScale(self, arg0: vec3) -> None:
        pass
    @property
    def CollisionAssetPath(self) -> str:
        """
        :type: str
        """
    @CollisionAssetPath.setter
    def CollisionAssetPath(self, arg0: str) -> None:
        pass
    @property
    def EnableCCD(self) -> bool:
        """
        :type: bool
        """
    @EnableCCD.setter
    def EnableCCD(self, arg0: bool) -> None:
        pass
    @property
    def IsTrigger(self) -> bool:
        """
        :type: bool
        """
    @IsTrigger.setter
    def IsTrigger(self, arg0: bool) -> None:
        pass
    @property
    def Shape(self) -> CollisionShape:
        """
        :type: CollisionShape
        """
    @Shape.setter
    def Shape(self, arg0: CollisionShape) -> None:
        pass
    @property
    def Type(self) -> HEngine.Rigidbody3DComponent.Body3DType:
        """
        :type: HEngine.Rigidbody3DComponent.Body3DType
        """
    @Type.setter
    def Type(self, arg0: HEngine.Rigidbody3DComponent.Body3DType) -> None:
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
class SceneCamera():
    class ProjectionType():
        """
        Members:

          Perspective

          Orthographic
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
        Orthographic: HEngine.SceneCamera.ProjectionType # value = <ProjectionType.Orthographic: 1>
        Perspective: HEngine.SceneCamera.ProjectionType # value = <ProjectionType.Perspective: 0>
        __members__: dict # value = {'Perspective': <ProjectionType.Perspective: 0>, 'Orthographic': <ProjectionType.Orthographic: 1>}
        pass
    def SetAspectRatio(self, aspect: float) -> None: ...
    def SetOrthographic(self, size: float, nearClip: float, farClip: float) -> None: ...
    def SetPerspective(self, verticalFOV: float, nearClip: float, farClip: float) -> None: ...
    def SetViewportSize(self, width: int, height: int) -> None: ...
    def __init__(self) -> None: ...
    @property
    def OrthographicFarClip(self) -> float:
        """
        :type: float
        """
    @OrthographicFarClip.setter
    def OrthographicFarClip(self, arg1: float) -> None:
        pass
    @property
    def OrthographicNearClip(self) -> float:
        """
        :type: float
        """
    @OrthographicNearClip.setter
    def OrthographicNearClip(self, arg1: float) -> None:
        pass
    @property
    def OrthographicSize(self) -> float:
        """
        :type: float
        """
    @OrthographicSize.setter
    def OrthographicSize(self, arg1: float) -> None:
        pass
    @property
    def PerspectiveFarClip(self) -> float:
        """
        :type: float
        """
    @PerspectiveFarClip.setter
    def PerspectiveFarClip(self, arg1: float) -> None:
        pass
    @property
    def PerspectiveNearClip(self) -> float:
        """
        :type: float
        """
    @PerspectiveNearClip.setter
    def PerspectiveNearClip(self, arg1: float) -> None:
        pass
    @property
    def PerspectiveVerticalFOV(self) -> float:
        """
        :type: float
        """
    @PerspectiveVerticalFOV.setter
    def PerspectiveVerticalFOV(self, arg1: float) -> None:
        pass
    @property
    def ProjectionTypeValue(self) -> SceneCamera.ProjectionType:
        """
        :type: SceneCamera.ProjectionType
        """
    @ProjectionTypeValue.setter
    def ProjectionTypeValue(self, arg1: SceneCamera.ProjectionType) -> None:
        pass
    pass
class ScriptComponent():
    def __init__(self) -> None: ...
    @property
    def Backend(self) -> str:
        """
        :type: str
        """
    @Backend.setter
    def Backend(self, arg0: str) -> None:
        pass
    @property
    def Enabled(self) -> bool:
        """
        :type: bool
        """
    @Enabled.setter
    def Enabled(self, arg0: bool) -> None:
        pass
    @property
    def Fields(self) -> str:
        """
        :type: str
        """
    @Fields.setter
    def Fields(self, arg0: str) -> None:
        pass
    @property
    def Type(self) -> str:
        """
        :type: str
        """
    @Type.setter
    def Type(self, arg0: str) -> None:
        pass
    pass
class SoundComponent():
    def __init__(self) -> None: ...
    @property
    def Attenuation(self) -> Audio.Attenuation:
        """
        :type: Audio.Attenuation
        """
    @Attenuation.setter
    def Attenuation(self, arg0: Audio.Attenuation) -> None:
        pass
    @property
    def Bus(self) -> Audio.Bus:
        """
        :type: Audio.Bus
        """
    @Bus.setter
    def Bus(self, arg0: Audio.Bus) -> None:
        pass
    @property
    def DopplerFactor(self) -> float:
        """
        :type: float
        """
    @DopplerFactor.setter
    def DopplerFactor(self, arg0: float) -> None:
        pass
    @property
    def MaxDistance(self) -> float:
        """
        :type: float
        """
    @MaxDistance.setter
    def MaxDistance(self, arg0: float) -> None:
        pass
    @property
    def MinDistance(self) -> float:
        """
        :type: float
        """
    @MinDistance.setter
    def MinDistance(self, arg0: float) -> None:
        pass
    @property
    def Path(self) -> str:
        """
        :type: str
        """
    @Path.setter
    def Path(self, arg0: str) -> None:
        pass
    @property
    def Pitch(self) -> float:
        """
        :type: float
        """
    @Pitch.setter
    def Pitch(self, arg0: float) -> None:
        pass
    @property
    def Play(self) -> bool:
        """
        :type: bool
        """
    @Play.setter
    def Play(self, arg0: bool) -> None:
        pass
    @property
    def Rolloff(self) -> float:
        """
        :type: float
        """
    @Rolloff.setter
    def Rolloff(self, arg0: float) -> None:
        pass
    @property
    def Volume(self) -> float:
        """
        :type: float
        """
    @Volume.setter
    def Volume(self, arg0: float) -> None:
        pass
    @property
    def bLooping(self) -> bool:
        """
        :type: bool
        """
    @bLooping.setter
    def bLooping(self, arg0: bool) -> None:
        pass
    @property
    def bSpatialized(self) -> bool:
        """
        :type: bool
        """
    @bSpatialized.setter
    def bSpatialized(self, arg0: bool) -> None:
        pass
    @property
    def bStream(self) -> bool:
        """
        :type: bool
        """
    @bStream.setter
    def bStream(self, arg0: bool) -> None:
        pass
    pass
class SpotLightComponent():
    def __init__(self) -> None: ...
    @property
    def InnerConeAngle(self) -> float:
        """
        :type: float
        """
    @InnerConeAngle.setter
    def InnerConeAngle(self, arg0: float) -> None:
        pass
    @property
    def Intensity(self) -> float:
        """
        :type: float
        """
    @Intensity.setter
    def Intensity(self, arg0: float) -> None:
        pass
    @property
    def IntensityUnits(self) -> int:
        """
        :type: int
        """
    @IntensityUnits.setter
    def IntensityUnits(self, arg0: int) -> None:
        pass
    @property
    def LightColor(self) -> vec3:
        """
        :type: vec3
        """
    @LightColor.setter
    def LightColor(self, arg0: vec3) -> None:
        pass
    @property
    def OuterConeAngle(self) -> float:
        """
        :type: float
        """
    @OuterConeAngle.setter
    def OuterConeAngle(self, arg0: float) -> None:
        pass
    @property
    def Radius(self) -> float:
        """
        :type: float
        """
    @Radius.setter
    def Radius(self, arg0: float) -> None:
        pass
    pass
class SpringArmComponent():
    def __init__(self) -> None: ...
    @property
    def ArmLength(self) -> float:
        """
        :type: float
        """
    @ArmLength.setter
    def ArmLength(self, arg0: float) -> None:
        pass
    @property
    def CollisionRecoverSpeed(self) -> float:
        """
        :type: float
        """
    @CollisionRecoverSpeed.setter
    def CollisionRecoverSpeed(self, arg0: float) -> None:
        pass
    @property
    def CollisionSnapSpeed(self) -> float:
        """
        :type: float
        """
    @CollisionSnapSpeed.setter
    def CollisionSnapSpeed(self, arg0: float) -> None:
        pass
    @property
    def LagSpeed(self) -> float:
        """
        :type: float
        """
    @LagSpeed.setter
    def LagSpeed(self, arg0: float) -> None:
        pass
    @property
    def MinArmLength(self) -> float:
        """
        :type: float
        """
    @MinArmLength.setter
    def MinArmLength(self, arg0: float) -> None:
        pass
    @property
    def ProbeRadius(self) -> float:
        """
        :type: float
        """
    @ProbeRadius.setter
    def ProbeRadius(self, arg0: float) -> None:
        pass
    @property
    def SocketOffset(self) -> vec3:
        """
        :type: vec3
        """
    @SocketOffset.setter
    def SocketOffset(self, arg0: vec3) -> None:
        pass
    @property
    def TargetOffset(self) -> vec3:
        """
        :type: vec3
        """
    @TargetOffset.setter
    def TargetOffset(self, arg0: vec3) -> None:
        pass
    @property
    def bEnableCollision(self) -> bool:
        """
        :type: bool
        """
    @bEnableCollision.setter
    def bEnableCollision(self, arg0: bool) -> None:
        pass
    @property
    def bEnableLag(self) -> bool:
        """
        :type: bool
        """
    @bEnableLag.setter
    def bEnableLag(self, arg0: bool) -> None:
        pass
    @property
    def bEnableTopDownCollision(self) -> bool:
        """
        :type: bool
        """
    @bEnableTopDownCollision.setter
    def bEnableTopDownCollision(self, arg0: bool) -> None:
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
    def Texture(self) -> str:
        """
        :type: str
        """
    @Texture.setter
    def Texture(self, arg0: str) -> None:
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
    def ParentUuid(self) -> str:
        """
        :type: str
        """
    @ParentUuid.setter
    def ParentUuid(self, arg0: str) -> None:
        pass
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
class UUID():
    def __eq__(self, arg0: object) -> bool: ...
    def __hash__(self) -> int: ...
    @typing.overload
    def __init__(self) -> None: ...
    @typing.overload
    def __init__(self, value: str) -> None: ...
    def __ne__(self, arg0: object) -> bool: ...
    def __str__(self) -> str: ...
    def isValid(self) -> bool: ...
    def str(self) -> str: ...
    pass
class _ComponentView():
    def __getattr__(self, arg0: str) -> object: ...
    def __getitem__(self, arg0: object) -> object: ...
    def __iter__(self) -> object: ...
    def __len__(self) -> int: ...
    def __repr__(self) -> str: ...
    def __setattr__(self, arg0: str, arg1: object) -> None: ...
    def __setitem__(self, arg0: object, arg1: object) -> None: ...
    pass
class mat4():
    @typing.overload
    def __init__(self, arg0: _ComponentView) -> None: ...
    @typing.overload
    def __init__(self, arg0: float) -> None: ...
    pass
class vec2():
    @typing.overload
    def __init__(self, arg0: _ComponentView) -> None: ...
    @typing.overload
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
    @typing.overload
    def __init__(self, arg0: _ComponentView) -> None: ...
    @typing.overload
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
    @typing.overload
    def __init__(self, arg0: _ComponentView) -> None: ...
    @typing.overload
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
def ApplySurvivorAgentHit(entity: Entity, damage: float, knock_x: float, knock_z: float) -> float:
    pass
def ApplySurvivorAgentStatus(entity: Entity, status_id: str, magnitude: float, duration: float, generation: int) -> tuple:
    pass
def DeactivateSurvivorAgent(entity: Entity) -> None:
    pass
def DeactivateSurvivorPickup(entity: Entity, activation_id: int) -> bool:
    pass
def Destroy(entity: Entity) -> None:
    pass
def FindEntityByTag(tag: str) -> Entity:
    pass
def FindInstanceEntity(instanceMember: Entity, localId: str) -> Entity:
    pass
def FindSurvivorAgentCircleOverlap(origin_x: float, origin_y: float, origin_z: float, padding: float, vertical_range: float) -> object:
    pass
def GetFullPath(arg0: str) -> str:
    pass
def GetSavedPath(arg0: str) -> str:
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
@typing.overload
def Instantiate(prefabPath: str) -> Entity:
    pass
@typing.overload
def Instantiate(prefabPath: str, position: vec3) -> Entity:
    pass
@typing.overload
def Instantiate(prefabPath: str, position: vec3, rotation: vec3) -> Entity:
    pass
def KillSurvivorAgent(entity: Entity) -> float:
    pass
def QuerySurvivorAgentCorridors(candidate_origin_x: float, candidate_origin_y: float, candidate_origin_z: float, candidate_range: float, candidate_vertical_range: float, corridor_specs: list) -> list:
    pass
def QuerySurvivorAgentsInRadius(origin_x: float, origin_y: float, origin_z: float, radius: float, vertical_range: float) -> list:
    pass
def QuerySurvivorAgentsInSweptAnnulus(origin_x: float, origin_y: float, origin_z: float, previous_radius: float, current_radius: float, edge_width: float, vertical_range: float) -> list:
    pass
def QuerySurvivorAgentsInSweptCapsule(start_x: float, start_z: float, end_x: float, end_z: float, sweep_radius: float) -> list:
    pass
def RaycastFloorHeight(position: vec3, cast_distance_above: float = 8.0, cast_distance_below: float = 24.0) -> object:
    pass
def RegisterSurvivorAgent(entity: Entity, monster: dict) -> int:
    pass
def RegisterSurvivorPickup(entity: Entity, activation_id: int, base_y: float, phase: float, rotation: float, scale_bonus: float) -> int:
    pass
def RequestRuntimeScene(arg0: str) -> bool:
    pass
def ResetSurvivorAgents() -> None:
    pass
def ResetSurvivorPickups() -> None:
    pass
def SelectDensestSurvivorAgents(origin_x: float, origin_y: float, origin_z: float, attack_range: float, vertical_range: float, cluster_radius: float, target_count: int) -> list:
    pass
def SelectSurvivorAgentChain(origin_x: float, origin_y: float, origin_z: float, attack_range: float, vertical_range: float, link_range: float, maximum_targets: int) -> list:
    pass
def SetRuntimePaused(arg0: bool) -> None:
    pass
def SetSurvivorAgentDistanceRetirement(entity: Entity, enabled: bool, generation: int) -> bool:
    pass
def SetSurvivorAgentHealth(entity: Entity, health: float, generation: int) -> bool:
    pass
def SetSurvivorPickupScaleBonus(entity: Entity, activation_id: int, scale_bonus: float) -> bool:
    pass
def UpdateSurvivorAgents(player_position: vec3, delta_time: float, elapsed: float, damage_cooldown: float, health: float, damage_reduction: float, random_state: int, max_animated_monsters: int, animation_lod_distance: float, animation_lod_release_distance: float, despawn_distance_squared: float, animation_controller_path: str) -> tuple:
    pass
def UpdateSurvivorCrowd(monsters: list, player_position: vec3, delta_time: float, elapsed: float, damage_cooldown: float, health: float, damage_reduction: float, random_state: int, max_animated_monsters: int, animation_lod_distance: float, animation_lod_release_distance: float, despawn_distance_squared: float, animation_controller_path: str) -> tuple:
    pass
def UpdateSurvivorPickups(player_position: vec3, delta_time: float, magnet_range: float, pickup_range: float, gem_scale: vec3) -> list:
    pass
