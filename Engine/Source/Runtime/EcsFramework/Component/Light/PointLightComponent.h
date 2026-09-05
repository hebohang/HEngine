#pragma once 

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace HEngine
{
    REFLECTION_TYPE(PointLightComponent)
    CLASS(PointLightComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(PointLightComponent);
    public:
        PointLightComponent() = default;
        PointLightComponent(const PointLightComponent&) = default;
        PointLightComponent(float intensity, const glm::vec3& lightColor, float radius = 10.0f, float falloffExponent = 2.0f)
            : Intensity(intensity), LightColor(lightColor), Radius(radius), FalloffExponent(falloffExponent) {}

        float Intensity = 800.0f;
        glm::vec3 LightColor = { 1.0f, 1.0f, 1.0f };
        float Radius = 10.0f;
        float FalloffExponent = 2.0f;
    };
    REGISTER_COMPONENT_TYPE(PointLightComponent)
}
