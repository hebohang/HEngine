#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
	REFLECTION_TYPE(Rigidbody2DComponent)
	CLASS(Rigidbody2DComponent : public ComponentBase, Fields)
	{
		REFLECTION_COMPONENT_BODY(Rigidbody2DComponent);
	public:
		enum class BodyType { Static = 0, Dynamic, Kinematic };
		BodyType Type = BodyType::Static;
		bool FixedRotation = false;

		// Storage for runtime
		PROPERTY(Disable)
		void* RuntimeBody = nullptr;

		Rigidbody2DComponent() = default;
		Rigidbody2DComponent(const Rigidbody2DComponent&) = default;
	};
	REGISTER_COMPONENT_TYPE(Rigidbody2DComponent)
}