#pragma once
#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/Renderer/Texture.h"

#include <glm/glm.hpp>

namespace HEngine
{
	class SpriteRendererComponent : public ComponentBase
	{
	public:
		glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		Ref<Texture2D> Texture;
		float TilingFactor = 1.0f;

		SpriteRendererComponent() = default;
		SpriteRendererComponent(const SpriteRendererComponent&) = default;
		SpriteRendererComponent(const glm::vec4 & color)
			: Color(color) {}
	};
}