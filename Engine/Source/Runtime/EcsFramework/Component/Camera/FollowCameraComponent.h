#pragma once

#include "Runtime/Core/UUID.h"
#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
    enum class FollowCameraMode
    {
        ThirdPerson = 0,
        TopDown
    };

    REFLECTION_TYPE(FollowCameraComponent)
    CLASS(FollowCameraComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(FollowCameraComponent);
    public:
        FollowCameraMode Mode = FollowCameraMode::ThirdPerson;

        bool bHasTarget = false;
        UUID TargetUUID;

        bool bAllowYawInput = true;
        bool bAllowPitchInput = true;
        bool bRequireHoldToRotate = true;
        bool bFollowTargetRotation = false;
        bool bLockToWorldUp = true;

        float MouseSensitivity = 0.005f;
        float InitialPitchDegrees = -12.0f;
        float MinPitchDegrees = -85.0f;
        float MaxPitchDegrees = 85.0f;

        float TopDownYawDegrees = 0.0f;
        float TopDownPitchDegrees = 60.0f;
        float TopDownDistance = 12.0f;

        PROPERTY(Disable)
        bool bRuntimeInitialized = false;

        PROPERTY(Disable)
        float RuntimeYawRadians = 0.0f;

        PROPERTY(Disable)
        float RuntimePitchRadians = 0.0f;

        PROPERTY(Disable)
        glm::vec2 RuntimeLastMousePos = glm::vec2(0.0f);

        PROPERTY(Disable)
        float RuntimeMissingTargetWarnCooldown = 0.0f;

        PROPERTY(Disable)
        float RuntimeMissingTransformWarnCooldown = 0.0f;

        FollowCameraComponent() = default;
        FollowCameraComponent(const FollowCameraComponent&) = default;
    };
    REGISTER_COMPONENT_TYPE(FollowCameraComponent)
}
