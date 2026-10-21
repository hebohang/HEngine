#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

namespace HEngine
{
	enum class CollisionShape
	{
		None = 0,
		Box = 1,
		Sphere,
		ConvexHull,
		TriangleMesh
	};

	REFLECTION_TYPE(Rigidbody3DComponent)
	CLASS(Rigidbody3DComponent : public ComponentBase, Fields)
	{
		REFLECTION_COMPONENT_BODY(Rigidbody3DComponent);
    public:
        static constexpr BuildFeature RequiredBuildFeature = BuildFeature::Physics3D;
	public:
		enum class Body3DType { Static = 0, Dynamic, Kinematic };
	public:
		Body3DType Type = Body3DType::Static;
		CollisionShape Shape = CollisionShape::Box;
		// Optional v1 cooked static triangle shape. Empty preserves all legacy shapes.
		std::string CollisionAssetPath;

		float mass{ 1.0f };
		float linearDamping = 0.0f;
		float angularDamping = 0.05f;
		float restitution = 0.3f;
		float friction = 0.5f;
		bool IsTrigger = false;
		bool EnableCCD = false;
		bool AllowSleeping = true;

		glm::vec3 ColliderOffset = glm::vec3(0.0f);
		glm::vec3 ColliderRotation = glm::vec3(0.0f);
		glm::vec3 ColliderScale = glm::vec3(1.0f);

		// Storage for runtime
		PROPERTY(Disable)
		void* RuntimeBody = nullptr;

		Rigidbody3DComponent() = default;
		Rigidbody3DComponent(const Rigidbody3DComponent&) = default;
        Rigidbody3DComponent& operator=(const Rigidbody3DComponent&) = default;
        Rigidbody3DComponent(Rigidbody3DComponent&&) noexcept = default;
        Rigidbody3DComponent& operator=(Rigidbody3DComponent&&) noexcept = default;
	};
	REGISTER_COMPONENT_TYPE(Rigidbody3DComponent)
}
