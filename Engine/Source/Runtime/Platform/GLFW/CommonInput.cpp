#include "Hepch.h"
#include "Runtime/Core/Input/Input.h"

#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Platform/GLFW/CommonWindow.h"
#include "Runtime/Platform/GLFW/GlfwInputEvents.h"
#include "Runtime/Platform/GLFW/GlfwEventModifiers.h"

#include <GLFW/glfw3.h>

namespace HEngine
{
    bool Input::SetTouchControlsEnabled(bool) { return false; }
	namespace
	{
		constexpr std::uint32_t kMaximumGamepads = GLFW_JOYSTICK_LAST - GLFW_JOYSTICK_1 + 1;

		[[nodiscard]] GLFWwindow* GetInputWindow()
		{
			auto* window = dynamic_cast<CommonWindow*>(&Application::GetWindow());
			HE_CORE_ASSERT(window && window->GetGlfwWindow(), "GLFW input requires CommonWindow");
			return window->GetGlfwWindow();
		}

		[[nodiscard]] int ToGlfwJoystick(std::uint32_t gamepadIndex)
		{
			return gamepadIndex < kMaximumGamepads
				? GLFW_JOYSTICK_1 + static_cast<int>(gamepadIndex)
				: -1;
		}
	}

    bool Input::IsKeyPressed(KeyCode key)
    {
#if defined(HE_DEV_AGENT_RUNTIME)
        switch (QuerySyntheticKey(key))
        {
        case SyntheticKeyQuery::Pressed:
            return true;
        case SyntheticKeyQuery::Released:
            return false;
        case SyntheticKeyQuery::UsePhysical:
            break;
        }
#endif
#if defined(__APPLE__)
        int modifiers = 0;
        if (TryGetGlfwEventModifiers(modifiers))
        {
            switch (key)
            {
            case KeyCode::LeftControl: case KeyCode::RightControl: return (modifiers & GLFW_MOD_CONTROL) != 0;
            case KeyCode::LeftShift: case KeyCode::RightShift: return (modifiers & GLFW_MOD_SHIFT) != 0;
            case KeyCode::LeftAlt: case KeyCode::RightAlt: return (modifiers & GLFW_MOD_ALT) != 0;
            case KeyCode::LeftSuper: case KeyCode::RightSuper: return (modifiers & GLFW_MOD_SUPER) != 0;
            default: break;
            }
        }
#endif
        GLFWwindow* window = GetInputWindow();
        auto state = glfwGetKey(window, static_cast<int32_t>(key));
        return state == GLFW_PRESS || state == GLFW_REPEAT;
    }
    bool Input::IsMouseButtonPressed(MouseCode button)
    {
#if defined(HE_DEV_AGENT_RUNTIME)
        const auto synthetic = QuerySyntheticMouseButton(button);
        if (synthetic != SyntheticKeyQuery::UsePhysical) return synthetic == SyntheticKeyQuery::Pressed;
#endif
        GLFWwindow* window = GetInputWindow();
        auto state = glfwGetMouseButton(window, static_cast<int32_t>(button));
        return state == GLFW_PRESS;
    }
    bool Input::IsMouseButtonReleased(MouseCode button)
    {
#if defined(HE_DEV_AGENT_RUNTIME)
        const auto synthetic = QuerySyntheticMouseButton(button);
        if (synthetic != SyntheticKeyQuery::UsePhysical) return synthetic == SyntheticKeyQuery::Released;
#endif
        GLFWwindow* window = GetInputWindow();
        auto state = glfwGetMouseButton(window, static_cast<int32_t>(button));
        return state == GLFW_RELEASE;
    }
    glm::vec2 Input::GetMousePosition()
    {
#if defined(HE_DEV_AGENT_RUNTIME)
        glm::vec2 syntheticPosition;
        if (TryGetSyntheticPointerPosition(syntheticPosition))
            return syntheticPosition;
#endif
        glm::vec2 eventPosition;
        if (TryGetGlfwMouseEventPosition(eventPosition)) return eventPosition;
        GLFWwindow* window = GetInputWindow();
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);

        return { (float)xpos, (float)ypos };
    }
    float Input::GetMouseX()
    {
        return GetMousePosition().x;
    }
    float Input::GetMouseY()
    {
        return GetMousePosition().y;
    }

    GamepadState Input::GetGamepadState(std::uint32_t gamepadIndex)
    {
        GamepadState result;
        const int joystick = ToGlfwJoystick(gamepadIndex);
        if (joystick < 0 || glfwJoystickIsGamepad(joystick) != GLFW_TRUE)
            return result;

        GLFWgamepadstate nativeState{};
        if (glfwGetGamepadState(joystick, &nativeState) != GLFW_TRUE)
            return result;

        result.bConnected = true;
        const auto copyButton = [&](GamepadButton button, int nativeButton)
        {
            result.Buttons[static_cast<std::size_t>(button)] =
                nativeState.buttons[nativeButton] == GLFW_PRESS;
        };
        copyButton(GamepadButton::A, GLFW_GAMEPAD_BUTTON_A);
        copyButton(GamepadButton::B, GLFW_GAMEPAD_BUTTON_B);
        copyButton(GamepadButton::X, GLFW_GAMEPAD_BUTTON_X);
        copyButton(GamepadButton::Y, GLFW_GAMEPAD_BUTTON_Y);
        copyButton(GamepadButton::LeftBumper, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
        copyButton(GamepadButton::RightBumper, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
        copyButton(GamepadButton::Back, GLFW_GAMEPAD_BUTTON_BACK);
        copyButton(GamepadButton::Start, GLFW_GAMEPAD_BUTTON_START);
        copyButton(GamepadButton::Guide, GLFW_GAMEPAD_BUTTON_GUIDE);
        copyButton(GamepadButton::LeftThumb, GLFW_GAMEPAD_BUTTON_LEFT_THUMB);
        copyButton(GamepadButton::RightThumb, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB);
        copyButton(GamepadButton::DPadUp, GLFW_GAMEPAD_BUTTON_DPAD_UP);
        copyButton(GamepadButton::DPadRight, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT);
        copyButton(GamepadButton::DPadDown, GLFW_GAMEPAD_BUTTON_DPAD_DOWN);
        copyButton(GamepadButton::DPadLeft, GLFW_GAMEPAD_BUTTON_DPAD_LEFT);

        const auto copyStickAxis = [&](GamepadAxis axis, int nativeAxis)
        {
            result.Axes[static_cast<std::size_t>(axis)] =
                std::clamp(nativeState.axes[nativeAxis], -1.0f, 1.0f);
        };
        copyStickAxis(GamepadAxis::LeftX, GLFW_GAMEPAD_AXIS_LEFT_X);
        copyStickAxis(GamepadAxis::LeftY, GLFW_GAMEPAD_AXIS_LEFT_Y);
        copyStickAxis(GamepadAxis::RightX, GLFW_GAMEPAD_AXIS_RIGHT_X);
        copyStickAxis(GamepadAxis::RightY, GLFW_GAMEPAD_AXIS_RIGHT_Y);
        result.Axes[static_cast<std::size_t>(GamepadAxis::LeftTrigger)] =
            std::clamp((nativeState.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f, 0.0f, 1.0f);
        result.Axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] =
            std::clamp((nativeState.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f, 0.0f, 1.0f);
        return result;
    }

    std::int32_t Input::FindFirstConnectedGamepad()
    {
        for (std::uint32_t index = 0; index < kMaximumGamepads; ++index)
        {
            const int joystick = ToGlfwJoystick(index);
            if (joystick >= 0 && glfwJoystickIsGamepad(joystick) == GLFW_TRUE)
                return static_cast<std::int32_t>(index);
        }
        return -1;
    }
}
