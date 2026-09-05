#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/Core/UUID.h"

namespace HEngine
{
	REFLECTION_TYPE(IDComponent)
	CLASS(IDComponent : public ComponentBase, Fields)
	{
		REFLECTION_COMPONENT_BODY(IDComponent);
	public:
		UUID ID;

		IDComponent() = default;
		IDComponent(const IDComponent&) = default;
		IDComponent(const UUID& id)
			: ID(id)
		{
		}
	};
}