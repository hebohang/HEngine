#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
	REFLECTION_TYPE(CircleCollider2DComponent)
	CLASS(CircleCollider2DComponent : public ComponentBase, Fields)
	{
		REFLECTION_COMPONENT_BODY(CircleCollider2DComponent);
	public:
		glm::vec2 Offset = { 0.0f, 0.0f };
		float Radius = 0.5f;

		// TODO: move into physics material in the future maybe 
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		float RestitutionThreshold = 0.5f;

		// Storage for runtime
		PROPERTY(Disable)
		void* RuntimeFixture = nullptr;

		CircleCollider2DComponent() = default;
		CircleCollider2DComponent(const CircleCollider2DComponent&) = default;
	};
	REGISTER_COMPONENT_TYPE(CircleCollider2DComponent)
}