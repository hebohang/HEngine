#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/Camera/SceneCamera.h"

namespace HEngine
{
    REFLECTION_TYPE(CameraComponent)
    CLASS(CameraComponent : public ComponentBase, Fields)
	{
        REFLECTION_COMPONENT_BODY(CameraComponent);
    public:
        SceneCamera Camera;
        PROPERTY(NoPython)
        Json RenderOverrides = Json::object();
        bool Primary = true; // TODO: think about moving to Scene
        bool FixedAspectRatio = false;
        bool bOverrideExposure = false;
        float ExposureEV100 = 14.0f;
        float ExposureCompensation = 0.0f;

        CameraComponent() = default;
        CameraComponent(const CameraComponent&) = default;
	};
    REGISTER_COMPONENT_TYPE(CameraComponent)
}
