#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <string>

namespace HEngine
{
    class TagComponent : public ComponentBase
    {
    public:
        std::string Tag;

        TagComponent() = default;
        TagComponent(const TagComponent&) = default;
        TagComponent(const std::string& tag)
            : Tag(tag) {}
    };
}