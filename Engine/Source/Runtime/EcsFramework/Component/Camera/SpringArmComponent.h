#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
    REFLECTION_TYPE(SpringArmComponent)
    CLASS(SpringArmComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(SpringArmComponent);
    public:
        float ArmLength = 3.2f;
        glm::vec3 SocketOffset = glm::vec3(0.5f, 1.6f, 0.0f);
        glm::vec3 TargetOffset = glm::vec3(0.0f);
        glm::vec3 LookAtOffset = glm::vec3(0.0f);

        bool bEnableLag = true;
        float LagSpeed = 12.0f;
        // Filter the shared orbit/look target, avoiding stair-induced pitch impulses.
        float VerticalLagSpeed = 8.0f;
        float LagResetDistance = 5.0f;
        PROPERTY(Disable)
        glm::vec3 RuntimePivot = glm::vec3(0.0f);
        PROPERTY(Disable)
        glm::vec3 RuntimePivotVelocity = glm::vec3(0.0f);

        bool bEnableCollision = true;
        float ProbeRadius = 0.2f;
        float MinArmLength = 0.6f;
        float CollisionRecoverSpeed = 8.0f;
        float CollisionSnapSpeed = 20.0f;
        bool bEnableTopDownCollision = false;

        PROPERTY(Disable)
        float RuntimeCurrentDistance = -1.0f;

        SpringArmComponent() = default;
        SpringArmComponent(const SpringArmComponent&) = default;
    };
    REGISTER_COMPONENT_TYPE(SpringArmComponent)
}
