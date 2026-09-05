#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace HEngine
{
    enum class GamepadButton : std::uint8_t
    {
        A,
        B,
        X,
        Y,
        LeftBumper,
        RightBumper,
        Back,
        Start,
        Guide,
        LeftThumb,
        RightThumb,
        DPadUp,
        DPadRight,
        DPadDown,
        DPadLeft,
        Count
    };

    enum class GamepadAxis : std::uint8_t
    {
        LeftX,
        LeftY,
        RightX,
        RightY,
        LeftTrigger,
        RightTrigger,
        Count
    };

    struct GamepadState
    {
        static constexpr std::size_t ButtonCount =
            static_cast<std::size_t>(GamepadButton::Count);
        static constexpr std::size_t AxisCount =
            static_cast<std::size_t>(GamepadAxis::Count);

        bool bConnected = false;
        std::array<bool, ButtonCount> Buttons{};
        std::array<float, AxisCount> Axes{};

        [[nodiscard]] bool IsButtonPressed(GamepadButton button) const noexcept
        {
            const std::size_t index = static_cast<std::size_t>(button);
            return index < Buttons.size() && Buttons[index];
        }

        [[nodiscard]] float GetAxis(GamepadAxis axis) const noexcept
        {
            const std::size_t index = static_cast<std::size_t>(axis);
            return index < Axes.size() ? Axes[index] : 0.0f;
        }
    };
}
