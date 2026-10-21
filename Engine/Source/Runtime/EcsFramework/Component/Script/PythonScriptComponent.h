#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
    REFLECTION_TYPE(PythonScriptComponent)
    CLASS(PythonScriptComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(PythonScriptComponent);
    public:
        PythonScriptComponent() = default;
        PythonScriptComponent(const PythonScriptComponent&) = default;
        PythonScriptComponent& operator=(const PythonScriptComponent&) = default;
        PythonScriptComponent(PythonScriptComponent&&) noexcept = default;
        PythonScriptComponent& operator=(PythonScriptComponent&&) noexcept = default;
		PythonScriptComponent(const std::string& path)
			: Path(path)
		{}

		std::string Path = "None";
	};
	REGISTER_COMPONENT_TYPE(PythonScriptComponent)
}
