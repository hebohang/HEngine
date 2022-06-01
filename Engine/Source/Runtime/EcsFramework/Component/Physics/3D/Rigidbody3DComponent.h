#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
	enum class CollisionShape
	{
		None = 0,
		Box = 1,
		Sphere,
		ConvexHull
	};

	class Rigidbody3DComponent : public ComponentBase
	{
	public:
		enum class Body3DType { Static = 0, Dynamic, Kinematic };
	public:
		Body3DType Type = Body3DType::Static;
		CollisionShape Shape = CollisionShape::Box;

		float mass{ 1.0f };
		float linearDamping = 0.0f;
		float angularDamping = 0.0f;
		float restitution = 1.0f;
		float friction = 1.0f;

		// Storage for runtime
		void* RuntimeBody = nullptr;

		Rigidbody3DComponent() = default;
		Rigidbody3DComponent(const Rigidbody3DComponent&) = default;
	};
}