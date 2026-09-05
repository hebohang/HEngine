#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <string>

namespace HEngine
{
    REFLECTION_TYPE(EnvironmentLightComponent)
    CLASS(EnvironmentLightComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(EnvironmentLightComponent);
    public:
        EnvironmentLightComponent() = default;
        EnvironmentLightComponent(const EnvironmentLightComponent&) = default;

        std::string HdrTexturePath;
        float Exposure = 0.8f;
        float IBLIntensity = 1.0f;
    };
    REGISTER_COMPONENT_TYPE(EnvironmentLightComponent)
}
