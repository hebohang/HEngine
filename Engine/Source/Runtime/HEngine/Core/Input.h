#pragma once

#include <glm/glm.hpp>

#include "HEngine/Core/KeyCodes.h"
#include "HEngine/Core/MouseCodes.h"

namespace HEngine
{
    class HENGINE_API Input
    {
    public:
        static bool IsKeyPressed(KeyCode key);

        static bool IsMouseButtonPressed(MouseCode button);
        static glm::vec2 GetMousePosition();
        static float GetMouseX();
        static float GetMouseY();
    };
}