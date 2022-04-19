#pragma once
#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/Scene/SceneCamera.h"

namespace HEngine
{
	class CameraComponent : public ComponentBase
	{
	public:
        SceneCamera Camera;
        bool Primary = true; // TODO: think about moving to Scene
        bool FixedAspectRatio = false;

        CameraComponent() = default;
        CameraComponent(const CameraComponent&) = default;
	};
}