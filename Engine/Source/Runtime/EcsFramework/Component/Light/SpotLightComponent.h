#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
    REFLECTION_TYPE(SpotLightComponent)
    CLASS(SpotLightComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(SpotLightComponent);
    public:
        SpotLightComponent() = default;
        SpotLightComponent(const SpotLightComponent&) = default;

        // Local -Z is the light's emission direction. Cone angles are half
        // angles in degrees; Radius is the finite spherical influence range.
        float Intensity = 800.0f;
        // 0: dimensionless (625 per cd), 1: cd, 2: lm within the outer cone.
        int IntensityUnits = 1;
        glm::vec3 LightColor = { 1.0f, 1.0f, 1.0f };
        float Radius = 10.0f;
        float InnerConeAngle = 20.0f;
        float OuterConeAngle = 30.0f;
    };
    REGISTER_COMPONENT_TYPE(SpotLightComponent)
}
