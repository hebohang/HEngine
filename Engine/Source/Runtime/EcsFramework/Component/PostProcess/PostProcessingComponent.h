#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
    REFLECTION_TYPE(PostProcessingComponent)
    CLASS(PostProcessingComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(PostProcessingComponent);
    public:
        PostProcessingComponent() = default;
        PostProcessingComponent(const PostProcessingComponent&) = default;

        bool bEnabled = true;
        // Global exposure applies to the whole view. Local bounds are evaluated
        // when explicitly enabled and the component has a transform.
        bool bGlobal = true;
        bool bLocalVolumeEnabled = false;
        float Priority = 0.0f;
        float BlendRadius = 0.0f;
        float BlendWeight = 1.0f;
        // Enabled volumes override exposure unless used only as spatial metadata.
        bool bOverrideExposure = true;
        float ExposureEV100 = 14.0f;
        float ExposureCompensation = 0.0f;
        // Additive schema evolution: old volumes inherit these settings unchanged.
        bool bOverrideBloomIntensity = false;
        float BloomIntensity = 0.05f;
        bool bOverrideContrast = false;
        float Contrast = 1.0f;
        bool bOverrideSaturation = false;
        float Saturation = 1.0f;
        bool bOverrideTemperature = false;
        float Temperature = 0.0f;
        bool bOverrideVignetteIntensity = false;
        float VignetteIntensity = 0.0f;
        bool bOverrideColorGain = false;
        glm::vec3 ColorGain{ 1.0f };
        bool bOverrideColorOffset = false;
        glm::vec3 ColorOffset{ 0.0f };
    };
    REGISTER_COMPONENT_TYPE(PostProcessingComponent)
}
