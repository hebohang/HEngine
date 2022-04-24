#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Physics/2D/BoxCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/2D/CircleCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/2D/Rigidbody2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/3D/BoxCollider3DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/3D/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/Script/NativeScriptComponent.h"
#include "Runtime/EcsFramework/Component/Shape/CircleRendererComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h" 
#include "Runtime/EcsFramework/Component/Mesh/StaticMeshComponent.h" 

#include <concepts>
#include <type_traits>

namespace HEngine
{
    // Every Component Class should be registered in this file

    template<class T>
    concept Component = std::is_base_of_v<ComponentBase, T>;

    template<Component... C>
    struct ComponentGroup
    {

    };

    using AllComponents = ComponentGroup<TransformComponent, CircleRendererComponent, SpriteRendererComponent,
        CameraComponent, NativeScriptComponent, Rigidbody2DComponent, BoxCollider2DComponent, 
        CircleCollider2DComponent, Rigidbody3DComponent, BoxCollider3DComponent, StaticMeshComponent>;
}