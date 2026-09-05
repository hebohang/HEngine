#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <string>

namespace HEngine
{
    REFLECTION_TYPE(TagComponent)
    CLASS(TagComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(TagComponent);
    public:
        std::string Tag;

        TagComponent() = default;
        TagComponent(const TagComponent&) = default;
        TagComponent(const std::string& tag)
            : Tag(tag) {}
    };
}