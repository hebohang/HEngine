#pragma once

#include "entt.hpp"

namespace HEngine
{
    class Scene
    {
    public:
        Scene();
        ~Scene();
    private:
        entt::registry m_Registry;
    };
}