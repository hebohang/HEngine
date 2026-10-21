#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

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
        float ExposureEV100 = 14.0f;
        float ExposureCompensation = 0.0f;
        float IBLIntensity = 1.0f;
        // Presentation-only brightness in stops; does not change diffuse/specular IBL.
        float SkyExposureCompensation = 0.0f;
        // Presentation-only linear sky color multiplier; IBL retains the source HDR.
        glm::vec3 SkyColorTint = glm::vec3(1.0f);

        bool bFogEnabled = false;
        glm::vec3 FogColor = glm::vec3(0.5f, 0.55f, 0.6f);
        float FogDensity = 0.0f;
        float FogStartDistance = 0.0f;
        float FogBaseHeight = 0.0f;
        float FogHeightFalloff = 0.0f;
        // An independent near-air layer; zero density preserves older scenes.
        glm::vec3 SecondaryFogColor = glm::vec3(0.0f);
        float SecondaryFogDensity = 0.0f;
        float SecondaryFogStartDistance = 0.0f;
        float SecondaryFogBaseHeight = 0.0f;
        float SecondaryFogHeightFalloff = 0.0f;
        // Optional sunward sky haze. Zero intensity preserves existing scenes.
        glm::vec3 FogDirectionalColor = glm::vec3(1.0f);
        float FogDirectionalIntensity = 0.0f;
        float FogDirectionalExponent = 4.0f;

        // Scene art controls; user quality is managed by RenderSettingsService.
        bool bCloudsEnabled = false;
        float CloudBottom = 90.0f;
        float CloudThickness = 95.0f;
        float CloudMaxDistance = 5000.0f;
        float CloudCoverage = 0.42f;
        float CloudDensity = 0.035f;
        float CloudScale = 260.0f;
        float CloudRoundness = 0.75f;
        float CloudErosion = 0.12f;
        unsigned int CloudSeed = 7301;
        float CloudWindAngle = 25.0f;
        float CloudWindSpeed = 1.8f;
        float CloudEvolutionSpeed = 0.015f;
        glm::vec3 CloudLitColor = glm::vec3(1.0f, 0.97f, 0.9f);
        glm::vec3 CloudShadowColor = glm::vec3(0.56f, 0.66f, 0.87f);
        float CloudAmbient = 0.65f;
        float CloudSilverLining = 0.3f;

    };
    REGISTER_COMPONENT_TYPE(EnvironmentLightComponent)
}
