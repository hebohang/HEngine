#pragma once

#include "Runtime/EcsFramework/Component/ComponentGroup.h"

#include <entt.hpp>

namespace HEngine
{
    class Registry final
    {
    public:
        Registry() = default;
        ~Registry() = default;
    public:

    private:
        entt::registry mRegistry;
    };
}