#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

#include <string>

namespace HEngine
{
	REFLECTION_TYPE(SpriteRendererComponent)
	CLASS(SpriteRendererComponent : public ComponentBase, Fields)
	{
		REFLECTION_COMPONENT_BODY(SpriteRendererComponent);
	public:
		glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };

		std::string Texture;

		float TilingFactor = 1.0f;

		SpriteRendererComponent() = default;
		SpriteRendererComponent(const SpriteRendererComponent&) = default;
		SpriteRendererComponent(const glm::vec4 & color)
			: Color(color) {}
	};
	REGISTER_COMPONENT_TYPE(SpriteRendererComponent)
}
