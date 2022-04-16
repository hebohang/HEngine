#pragma once

#include <glm/glm.hpp>

#include "Runtime/Core/KeyCodes.h"
#include "Runtime/Core/MouseCodes.h"

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