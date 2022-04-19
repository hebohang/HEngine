#pragma once
#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Physics/BoxCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/CircleCollider2DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/Rigidbody2DComponent.h"
#include "Runtime/EcsFramework/Component/Script/NativeScriptComponent.h"
#include "Runtime/EcsFramework/Component/Shape/CircleRendererComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h" 

#include <concepts>
#include <type_traits>

namespace HEngine
{
    //template<class T>
    //concept Component = std::is_base_o

    template<typename... Component>
    struct ComponentGroup
    {

    };
}