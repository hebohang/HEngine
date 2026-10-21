#pragma once
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/EcsFramework/Component/RenderSettingsComponent.h"

#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Basic/PrefabInstanceComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/FollowCameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/SpringArmComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#if WITH_EDITOR
#include "Runtime/EcsFramework/Component/Editor/EditorEntityStateComponent.h"
#include "Runtime/EcsFramework/Component/Editor/SceneFolderComponent.h"
#endif
#include "Runtime/EcsFramework/Component/Physics/TwoD/BoxCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/TwoD/CircleCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/TwoD/Rigidbody2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/CapsuleCollider3DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/Script/NativeScriptComponent.h"
#include "Runtime/EcsFramework/Component/Script/PythonScriptComponent.h"
#include "Runtime/EcsFramework/Component/Shape/CircleRendererComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h" 
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h" 
#include "Runtime/EcsFramework/Component/PostProcess/PostProcessingComponent.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Animation/SequenceComponent.h"
#include "Runtime/EcsFramework/Component/Animation/BoneAttachmentComponent.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/DirectionalLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/SpotLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/ReflectionProbeComponent.h"
#include "Runtime/EcsFramework/Component/Light/BakedIrradianceVolumeComponent.h"
#include "Runtime/EcsFramework/Component/Audio/SoundComponent.h"
#include "Runtime/EcsFramework/Component/Audio/AudioListenerComponent.h"
#include "Runtime/EcsFramework/Component/AI/BehaviorTreeComponent.h"

namespace HEngine
{
    // without IDComponent TagComponent and TransformComponent
    using ComponentsCommon       = _GET_COMPONENT_REGISTERED_TYPES();

    // we will first deserialized id and name for create an entity, so ComponentsDeserialized is without IDComponent and TagComponent
    using ComponentsDeserialized = Append<ComponentsCommon, TransformComponent>::type;
    using ComponentsCopied       = ComponentsDeserialized;

    // id is for the key, so ComponentsSerialized is without IDComponent
    using ComponentsSerialized   = Append<ComponentsDeserialized, TagComponent>::type;
    using ComponentsPyBinding    = ComponentsSerialized;

    // All components
    using AllComponents          = Append<ComponentsSerialized, IDComponent>::type;
}
