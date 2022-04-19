#pragma once
#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
	class CircleCollider2DComponent : public ComponentBase
	{
	public:
		glm::vec2 Offset = { 0.0f, 0.0f };
		float Radius = 0.5f;

		// TODO: move into physics material in the future maybe 
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		float RestitutionThreshold = 0.5f;

		// Storage for runtime
		void* RuntimeFixture = nullptr;

		CircleCollider2DComponent() = default;
		CircleCollider2DComponent(const CircleCollider2DComponent&) = default;
	};
}