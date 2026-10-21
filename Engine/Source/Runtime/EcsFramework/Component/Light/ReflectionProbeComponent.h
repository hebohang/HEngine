#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

#include <string>

namespace HEngine
{
    REFLECTION_TYPE(ReflectionProbeComponent)
    CLASS(ReflectionProbeComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(ReflectionProbeComponent);
    public:
        ReflectionProbeComponent() = default;
        ReflectionProbeComponent(const ReflectionProbeComponent&) = default;

        // Project-relative, linear HDR equirectangular asset captured at
        // CaptureOffset. Existing environment preprocessing produces the cube.
        std::string HdrTexturePath;
        glm::vec3 BoxHalfExtents{ 5.0f };
        glm::vec3 CaptureOffset{ 0.0f };
        float BlendDistance = 1.0f;
        float Intensity = 1.0f;
        bool bEnabled = true;
        // Disable when a baked SH volume already supplies local diffuse light.
        bool bAffectDiffuse = true;
    };
    REGISTER_COMPONENT_TYPE(ReflectionProbeComponent)
}
