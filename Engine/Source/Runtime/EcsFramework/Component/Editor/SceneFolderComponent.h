#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
    REFLECTION_TYPE(SceneFolderComponent)
    CLASS(SceneFolderComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(SceneFolderComponent);
    public:
        SceneFolderComponent() = default;
        SceneFolderComponent(const SceneFolderComponent&) = default;

        bool bExpanded = true;
    };
    REGISTER_COMPONENT_TYPE(SceneFolderComponent)
}
