#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
    REFLECTION_TYPE(CharacterMovementComponent)
    CLASS(CharacterMovementComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(CharacterMovementComponent);
    public:
        float MaxSpeed = 4.5f;
        float SprintSpeedMultiplier = 1.8f;
        float Acceleration = 25.0f;
        float SprintAccelerationMultiplier = 1.15f;
        float Deceleration = 20.0f;
        float TurnRate = 10.0f;

        bool bEnableSprint = true;
        bool bUseWorldUp = true;
        bool bOrientRotationToMovement = true;
        bool bPlayerInputEnabled = true;

        float MaxSlopeAngleDegrees = 50.0f;
        float StepUpHeight = 0.4f;
        float StickToFloorDistance = 0.5f;
        float Gravity = -9.8f;
        float JumpSpeed = 5.5f;
        bool bEnableJump = true;

        PROPERTY(Disable)
        glm::vec3 RuntimeVelocity = glm::vec3(0.0f);

        PROPERTY(Disable)
        void* RuntimeCharacter = nullptr;

        PROPERTY(Disable)
        glm::vec3 RuntimeDesiredVelocity = glm::vec3(0.0f);

        PROPERTY(Disable)
        bool RuntimeHasExternalVelocity = false;

        PROPERTY(Disable)
        float RuntimeAnimDirection = 0.0f;

        PROPERTY(Disable)
        float RuntimeLastInputYaw = 0.0f;

        PROPERTY(Disable)
        bool RuntimeHasLastInputYaw = false;

        PROPERTY(Disable)
        bool RuntimeJumpHeld = false;

        CharacterMovementComponent() = default;
        CharacterMovementComponent(const CharacterMovementComponent&) = default;
    };
    REGISTER_COMPONENT_TYPE(CharacterMovementComponent)
}
