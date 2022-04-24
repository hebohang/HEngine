#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
	class Rigidbody3DComponent : public ComponentBase
	{
	public:
		enum class Body3DType { Static = 0, Dynamic, Kinematic };
		Body3DType Type = Body3DType::Static;
		float mass{ 1.0f };

		// Storage for runtime
		void* RuntimeBody = nullptr;

		Rigidbody3DComponent() = default;
		Rigidbody3DComponent(const Rigidbody3DComponent&) = default;
	};
}