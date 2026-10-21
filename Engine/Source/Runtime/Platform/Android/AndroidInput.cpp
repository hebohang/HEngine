#include "AndroidContext.h"
#include "Runtime/Core/Input/Input.h"

namespace HEngine
{
    namespace
    {
        AndroidInputSnapshot ReadInput()
        {
            auto& state = GetAndroidContext();
            std::lock_guard lock(state.Mutex);
            return state.Input;
        }
    }
    bool Input::IsKeyPressed(KeyCode key)
    {
        const auto input = ReadInput();
        return (key == KeyCode::Space && input.Jump) || (key == KeyCode::Escape && input.Pause);
    }
    bool Input::IsMouseButtonPressed(MouseCode button)
    {
        const auto input = ReadInput();
        return (button == MouseCode::ButtonLeft && input.Primary) ||
            (button == MouseCode::ButtonRight && input.Secondary);
    }
    bool Input::IsMouseButtonReleased(MouseCode button) { return !IsMouseButtonPressed(button); }
    glm::vec2 Input::GetMousePosition() { const auto input = ReadInput(); return {input.X, input.Y}; }
    float Input::GetMouseX() { return GetMousePosition().x; }
    float Input::GetMouseY() { return GetMousePosition().y; }
    GamepadState Input::GetGamepadState(std::uint32_t index)
    {
        GamepadState gamepad;
        if (index != 0) return gamepad;
        const auto input = ReadInput();
        gamepad.bConnected = true;
        gamepad.Axes[static_cast<size_t>(GamepadAxis::LeftX)] = input.MoveX;
        gamepad.Axes[static_cast<size_t>(GamepadAxis::LeftY)] = -input.MoveY;
        gamepad.Buttons[static_cast<size_t>(GamepadButton::A)] = input.Jump;
        gamepad.Buttons[static_cast<size_t>(GamepadButton::Start)] = input.Pause;
        return gamepad;
    }
    std::int32_t Input::FindFirstConnectedGamepad() { return 0; }
    bool Input::SetTouchControlsEnabled(bool enabled)
    {
        auto& state = GetAndroidContext();
        std::lock_guard lock(state.Mutex);
        state.TouchControls = enabled;
        if (!enabled) { state.Input = state.PendingInput = {}; state.InputEvents.clear(); }
        return true;
    }
}
