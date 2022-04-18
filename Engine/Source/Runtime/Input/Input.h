#pragma once

#include <glm/glm.hpp>

#include "Runtime/Input/KeyCodes.h"
#include "Runtime/Input/MouseCodes.h"

namespace HEngine
{
    class HENGINE_API Input
    {
    public:
        static bool IsKeyPressed(KeyCode key);

        static bool IsMouseButtonPressed(MouseCode button);
        static glm::vec2 GetMousePosition();
        [[nodiscard]] static float GetMouseX();
        [[nodiscard]] static float GetMouseY();
    };
}