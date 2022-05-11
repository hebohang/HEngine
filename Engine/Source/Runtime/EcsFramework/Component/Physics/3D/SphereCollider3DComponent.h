#pragma once 

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
	class SphereCollider3DComponent : public ComponentBase
	{
	public:
		float radius = 1.0f;

		float linearDamping = 0.0f;
		float angularDamping = 0.0f;
		float restitution = 1.0f;
		float friction = 1.0f;

		SphereCollider3DComponent() = default;
		SphereCollider3DComponent(const SphereCollider3DComponent&) = default;
	};
}