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
        PointLightComponent(float intensity, const glm::vec3& lightColor, float radius = 10.0f)
            : Intensity(intensity), LightColor(lightColor), Radius(radius) {}

        float Intensity = 800.0f;
        // 0: dimensionless (625 per cd), 1: cd, 2: lm. Distances are meters.
        int IntensityUnits = 1;
        glm::vec3 LightColor = { 1.0f, 1.0f, 1.0f };
        float Radius = 10.0f;
    };
    REGISTER_COMPONENT_TYPE(PointLightComponent)
}
