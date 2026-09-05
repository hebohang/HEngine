#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
	REFLECTION_TYPE(BoxCollider2DComponent)
	CLASS(BoxCollider2DComponent : public ComponentBase, Fields)
	{
		REFLECTION_COMPONENT_BODY(BoxCollider2DComponent);
	public:
		glm::vec2 Offset = { 0.0f, 0.0f };
		glm::vec2 Size = { 0.5f, 0.5f };

		// TODO: move into physics material in the future maybe 
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		float RestitutionThreshold = 0.5f;

		// Storage for runtime
		PROPERTY(Disable)
		void* RuntimeFixture = nullptr;

		BoxCollider2DComponent() = default;
		BoxCollider2DComponent(const BoxCollider2DComponent&) = default;
	};
	REGISTER_COMPONENT_TYPE(BoxCollider2DComponent)
}