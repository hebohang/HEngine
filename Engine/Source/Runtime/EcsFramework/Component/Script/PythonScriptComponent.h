#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
    class PythonScriptComponent : public ComponentBase
    {
    public:
        PythonScriptComponent() = default;
        PythonScriptComponent(const PythonScriptComponent&) = default;
        PythonScriptComponent(const std::string& path)
            : Path(path)
        {}

        std::string Path;
    };
}