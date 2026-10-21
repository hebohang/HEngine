#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
    REFLECTION_TYPE(ScriptComponent)
    CLASS(ScriptComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(ScriptComponent);
    public:
        std::string Backend = "he.script.csharp";
        std::string Type;
        // Versioned typed payload; kept intact when a plugin or type is missing.
        std::string Fields = "{\"schema\":1,\"fields\":{}}";
        bool Enabled = true;
    };
    REGISTER_COMPONENT_TYPE(ScriptComponent)
}
