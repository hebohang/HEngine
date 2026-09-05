#pragma once 

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace HEngine
{
    REFLECTION_TYPE(DirectionalLightComponent)
    CLASS(DirectionalLightComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(DirectionalLightComponent);
    public:
        DirectionalLightComponent() = default;
        DirectionalLightComponent(const DirectionalLightComponent&) = default;
        DirectionalLightComponent(float intensity, const glm::vec3& color = glm::vec3(1.0f, 1.0f, 1.0f))
            : Intensity(intensity), Color(color) {}

        float Intensity = 3.0f;
        glm::vec3 Color = { 1.0f, 1.0f, 1.0f };

        bool bCastShadows = true;
        int CascadeCount = 4;
        int ShadowMapSize = 2048;
        float ShadowDistance = 150.0f;
        float CascadeLambda = 0.75f;
        float DepthBias = 0.0015f;
        float NormalBias = 0.05f;
        bool bUsePCF = true;
        float PCFRadius = 1.5f;
        float ShadowStrength = 1.0f;
    };
    REGISTER_COMPONENT_TYPE(DirectionalLightComponent)
}
