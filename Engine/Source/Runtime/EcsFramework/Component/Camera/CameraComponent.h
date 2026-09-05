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
        bool Primary = true; // TODO: think about moving to Scene
        bool FixedAspectRatio = false;
        bool bOverrideExposure = false;
        float Exposure = 0.8f;

        CameraComponent() = default;
        CameraComponent(const CameraComponent&) = default;
	};
    REGISTER_COMPONENT_TYPE(CameraComponent)
}
