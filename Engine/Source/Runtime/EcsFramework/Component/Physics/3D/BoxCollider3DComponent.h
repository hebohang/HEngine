#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
	class BoxCollider3DComponent : public ComponentBase
	{
	public:
		glm::vec3 Size = { 1.0f, 1.0f, 1.0f };

		float linearDamping = 0.0f;
		float angularDamping = 0.0f;
		float restitution = 1.0f;
		float friction = 1.0f;

		BoxCollider3DComponent() = default;
		BoxCollider3DComponent(const BoxCollider3DComponent&) = default;
	};
}