#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
    REFLECTION_TYPE(CapsuleCollider3DComponent)
    CLASS(CapsuleCollider3DComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(CapsuleCollider3DComponent);
    public:
        static constexpr BuildFeature RequiredBuildFeature = BuildFeature::Physics3D;
    public:
        glm::vec3 Center = glm::vec3(0.0f);
        float Radius = 0.35f;
        float Height = 1.8f;
        bool IsTrigger = false;

        PROPERTY(Disable)
        void* RuntimeShape = nullptr;

        CapsuleCollider3DComponent() = default;
        CapsuleCollider3DComponent(const CapsuleCollider3DComponent&) = default;
    };
    REGISTER_COMPONENT_TYPE(CapsuleCollider3DComponent)
}
