#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
	class BoxCollider3DComponent : public ComponentBase
	{
	public:
		glm::vec3 Size = { 0.5f, 0.5f, 0.5f };
		glm::vec3 inertia = { 0.0, 0.0, 0.0 };

		// TODO: move into physics material in the future maybe 
		float linearDamping = 0.0f;
		float angularDamping = 0.0f;
		float restitution = 0.5f;
		float friction = 0.5f;

		// Storage for runtime
		void* RuntimeFixture = nullptr;

		BoxCollider3DComponent() = default;
		BoxCollider3DComponent(const BoxCollider3DComponent&) = default;
	};
}