#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

#include <vector>

namespace HEngine
{
    // World-aligned, locally authored irradiance volume. SH stores cosine-convolved
    // linear irradiance, not exposed color or albedo. Rotation is deliberately not
    // supported: rebake after changing the world-space capture/bounds.
    REFLECTION_TYPE(BakedIrradianceVolumeComponent)
    CLASS(BakedIrradianceVolumeComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(BakedIrradianceVolumeComponent);
    public:
        glm::vec3 BoundsMin{ -5.0f };
        glm::vec3 BoundsMax{ 5.0f };
        std::vector<glm::vec3> IrradianceSH;
        float BlendDistance = 1.0f;
        float Intensity = 1.0f;
        bool bEnabled = true;
    };
    REGISTER_COMPONENT_TYPE(BakedIrradianceVolumeComponent)
}
