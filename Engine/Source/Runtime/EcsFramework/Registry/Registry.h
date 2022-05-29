#pragma once

#include <entt.hpp>

namespace HEngine
{
    // TODO
    // A wrapper of entt::registry

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