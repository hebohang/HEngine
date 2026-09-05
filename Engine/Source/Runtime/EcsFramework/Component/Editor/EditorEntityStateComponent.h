#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <string>

namespace HEngine
{
    REFLECTION_TYPE(EditorEntityStateComponent)
    CLASS(EditorEntityStateComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(EditorEntityStateComponent);
    public:
        EditorEntityStateComponent() = default;
        EditorEntityStateComponent(const EditorEntityStateComponent&) = default;

        bool bVisible = true;
        bool bLocked = false;
        std::string FolderUuid;
    };
    REGISTER_COMPONENT_TYPE(EditorEntityStateComponent)
}
