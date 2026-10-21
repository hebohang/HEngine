#pragma once

#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <string>
#include <vector>

namespace HEngine
{
    REFLECTION_TYPE(InputBindingRes)
    STRUCT(InputBindingRes, Fields)
    {
        REFLECTION_BODY(InputBindingRes);
        // Key, MouseButton, GamepadButton, GamepadAxis, PointerX, PointerY.
        std::string Device = "Key";
        int Code = 0;
        int Axis = 0;
        float Scale = 1.0f;
    };

    REFLECTION_TYPE(InputActionRes)
    STRUCT(InputActionRes, Fields)
    {
        REFLECTION_BODY(InputActionRes);
        std::string Name;
        std::string Context = "Gameplay";
        // Button, Axis, Vector2. Axis actions retain their signed value.
        std::string Type = "Button";
        float DeadZone = 0.15f;
        float HoldSeconds = 0.4f;
        float DoubleTapSeconds = 0.25f;
        std::vector<InputBindingRes> Bindings;
    };

    REFLECTION_TYPE(InputMapRes)
    STRUCT(InputMapRes, Fields)
    {
        REFLECTION_BODY(InputMapRes);
        int Version = 1;
        std::vector<InputActionRes> Actions;
    };
}
