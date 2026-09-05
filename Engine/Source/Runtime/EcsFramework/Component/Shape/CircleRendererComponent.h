#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
	REFLECTION_TYPE(CircleRendererComponent)
	CLASS(CircleRendererComponent : public ComponentBase, Fields)
	{
		REFLECTION_COMPONENT_BODY(CircleRendererComponent);
	public:
		glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		float Radius = 0.5f;
		float Thickness = 1.0f;
		float Fade = 0.005f;

		CircleRendererComponent() = default;
		CircleRendererComponent(const CircleRendererComponent&) = default;
	};
	REGISTER_COMPONENT_TYPE(CircleRendererComponent)
}