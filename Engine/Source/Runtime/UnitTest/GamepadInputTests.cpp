#include "Hepch.h"

#include "Runtime/UnitTest/GamepadInputTests.h"
#include "Runtime/Core/Input/GamepadCodes.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }
    }

    void GamepadInputStateContract()
    {
        static_assert(GamepadState::ButtonCount == 15);
        static_assert(GamepadState::AxisCount == 6);

        GamepadState state;
        Require(!state.bConnected, "A default gamepad snapshot reported connected.");
        Require(!state.IsButtonPressed(GamepadButton::A),
            "A default gamepad snapshot retained a pressed button.");
        Require(state.GetAxis(GamepadAxis::RightX) == 0.0f,
            "A default gamepad snapshot retained an axis value.");

        state.bConnected = true;
        state.Buttons[static_cast<std::size_t>(GamepadButton::A)] = true;
        state.Buttons[static_cast<std::size_t>(GamepadButton::DPadLeft)] = true;
        state.Axes[static_cast<std::size_t>(GamepadAxis::RightX)] = 0.75f;
        state.Axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] = 1.0f;
        Require(state.IsButtonPressed(GamepadButton::A),
            "The standardized south-face button was lost.");
        Require(state.IsButtonPressed(GamepadButton::DPadLeft),
            "The standardized D-pad state was lost.");
        Require(state.GetAxis(GamepadAxis::RightX) == 0.75f,
            "The standardized right-stick axis changed.");
        Require(state.GetAxis(GamepadAxis::RightTrigger) == 1.0f,
            "The normalized trigger axis changed.");
        Require(!state.IsButtonPressed(static_cast<GamepadButton>(255)),
            "An invalid gamepad button index was accepted.");
        Require(state.GetAxis(static_cast<GamepadAxis>(255)) == 0.0f,
            "An invalid gamepad axis index was accepted.");
    }
}
