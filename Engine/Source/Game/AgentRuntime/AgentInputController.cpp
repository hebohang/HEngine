#include "Game/AgentRuntime/AgentInputController.h"

#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Input/MouseCodes.h"
#include "Runtime/Events/MouseEvent.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace HEngine
{
    namespace
    {
        constexpr std::size_t MaxHeldKeys = 32;

        struct KeyBinding
        {
            std::string_view Name;
            KeyCode Code;
        };

        // This explicit protocol table is deliberately independent of GLFW/Win32
        // names. Adding a KeyCode does not silently expand the remote-control API.
        constexpr std::array KeyBindings{
            KeyBinding{ "space", KeyCode::Space },
            KeyBinding{ "apostrophe", KeyCode::Apostrophe },
            KeyBinding{ "comma", KeyCode::Comma },
            KeyBinding{ "minus", KeyCode::Minus },
            KeyBinding{ "period", KeyCode::Period },
            KeyBinding{ "slash", KeyCode::Slash },
            KeyBinding{ "0", KeyCode::D0 }, KeyBinding{ "1", KeyCode::D1 },
            KeyBinding{ "2", KeyCode::D2 }, KeyBinding{ "3", KeyCode::D3 },
            KeyBinding{ "4", KeyCode::D4 }, KeyBinding{ "5", KeyCode::D5 },
            KeyBinding{ "6", KeyCode::D6 }, KeyBinding{ "7", KeyCode::D7 },
            KeyBinding{ "8", KeyCode::D8 }, KeyBinding{ "9", KeyCode::D9 },
            KeyBinding{ "semicolon", KeyCode::Semicolon },
            KeyBinding{ "equal", KeyCode::Equal },
            KeyBinding{ "a", KeyCode::A }, KeyBinding{ "b", KeyCode::B },
            KeyBinding{ "c", KeyCode::C }, KeyBinding{ "d", KeyCode::D },
            KeyBinding{ "e", KeyCode::E }, KeyBinding{ "f", KeyCode::F },
            KeyBinding{ "g", KeyCode::G }, KeyBinding{ "h", KeyCode::H },
            KeyBinding{ "i", KeyCode::I }, KeyBinding{ "j", KeyCode::J },
            KeyBinding{ "k", KeyCode::K }, KeyBinding{ "l", KeyCode::L },
            KeyBinding{ "m", KeyCode::M }, KeyBinding{ "n", KeyCode::N },
            KeyBinding{ "o", KeyCode::O }, KeyBinding{ "p", KeyCode::P },
            KeyBinding{ "q", KeyCode::Q }, KeyBinding{ "r", KeyCode::R },
            KeyBinding{ "s", KeyCode::S }, KeyBinding{ "t", KeyCode::T },
            KeyBinding{ "u", KeyCode::U }, KeyBinding{ "v", KeyCode::V },
            KeyBinding{ "w", KeyCode::W }, KeyBinding{ "x", KeyCode::X },
            KeyBinding{ "y", KeyCode::Y }, KeyBinding{ "z", KeyCode::Z },
            KeyBinding{ "left_bracket", KeyCode::LeftBracket },
            KeyBinding{ "backslash", KeyCode::Backslash },
            KeyBinding{ "right_bracket", KeyCode::RightBracket },
            KeyBinding{ "grave_accent", KeyCode::GraveAccent },
            KeyBinding{ "escape", KeyCode::Escape },
            KeyBinding{ "enter", KeyCode::Enter },
            KeyBinding{ "tab", KeyCode::Tab },
            KeyBinding{ "backspace", KeyCode::Backspace },
            KeyBinding{ "insert", KeyCode::Insert },
            KeyBinding{ "delete", KeyCode::Delete },
            KeyBinding{ "right", KeyCode::Right },
            KeyBinding{ "left", KeyCode::Left },
            KeyBinding{ "down", KeyCode::Down },
            KeyBinding{ "up", KeyCode::Up },
            KeyBinding{ "page_up", KeyCode::PageUp },
            KeyBinding{ "page_down", KeyCode::PageDown },
            KeyBinding{ "home", KeyCode::Home },
            KeyBinding{ "end", KeyCode::End },
            KeyBinding{ "caps_lock", KeyCode::CapsLock },
            KeyBinding{ "scroll_lock", KeyCode::ScrollLock },
            KeyBinding{ "num_lock", KeyCode::NumLock },
            KeyBinding{ "print_screen", KeyCode::PrintScreen },
            KeyBinding{ "pause", KeyCode::Pause },
            KeyBinding{ "f1", KeyCode::F1 }, KeyBinding{ "f2", KeyCode::F2 },
            KeyBinding{ "f3", KeyCode::F3 }, KeyBinding{ "f4", KeyCode::F4 },
            KeyBinding{ "f5", KeyCode::F5 }, KeyBinding{ "f6", KeyCode::F6 },
            KeyBinding{ "f7", KeyCode::F7 }, KeyBinding{ "f8", KeyCode::F8 },
            KeyBinding{ "f9", KeyCode::F9 }, KeyBinding{ "f10", KeyCode::F10 },
            KeyBinding{ "f11", KeyCode::F11 }, KeyBinding{ "f12", KeyCode::F12 },
            KeyBinding{ "kp_0", KeyCode::KP0 }, KeyBinding{ "kp_1", KeyCode::KP1 },
            KeyBinding{ "kp_2", KeyCode::KP2 }, KeyBinding{ "kp_3", KeyCode::KP3 },
            KeyBinding{ "kp_4", KeyCode::KP4 }, KeyBinding{ "kp_5", KeyCode::KP5 },
            KeyBinding{ "kp_6", KeyCode::KP6 }, KeyBinding{ "kp_7", KeyCode::KP7 },
            KeyBinding{ "kp_8", KeyCode::KP8 }, KeyBinding{ "kp_9", KeyCode::KP9 },
            KeyBinding{ "kp_decimal", KeyCode::KPDecimal },
            KeyBinding{ "kp_divide", KeyCode::KPDivide },
            KeyBinding{ "kp_multiply", KeyCode::KPMultiply },
            KeyBinding{ "kp_subtract", KeyCode::KPSubtract },
            KeyBinding{ "kp_add", KeyCode::KPAdd },
            KeyBinding{ "kp_enter", KeyCode::KPEnter },
            KeyBinding{ "kp_equal", KeyCode::KPEqual },
            KeyBinding{ "left_shift", KeyCode::LeftShift },
            KeyBinding{ "left_control", KeyCode::LeftControl },
            KeyBinding{ "left_alt", KeyCode::LeftAlt },
            KeyBinding{ "left_super", KeyCode::LeftSuper },
            KeyBinding{ "right_shift", KeyCode::RightShift },
            KeyBinding{ "right_control", KeyCode::RightControl },
            KeyBinding{ "right_alt", KeyCode::RightAlt },
            KeyBinding{ "right_super", KeyCode::RightSuper },
            KeyBinding{ "menu", KeyCode::Menu }
        };

        [[nodiscard]] const KeyBinding* FindKey(std::string_view name)
        {
            const auto iterator = std::find_if(
                KeyBindings.begin(), KeyBindings.end(),
                [name](const KeyBinding& binding) { return binding.Name == name; });
            return iterator == KeyBindings.end() ? nullptr : &*iterator;
        }

        void RequireNoParams(const Json& params, std::string_view method)
        {
            if (!params.empty())
                throw std::invalid_argument(std::string(method) + " does not accept parameters.");
        }
    }

    void AgentInputController::Start()
    {
        mHeldKeyNames.clear();
        mVirtualClickCount = 0;
        Input::BeginSyntheticKeyboardSession(true);
        mActive = true;
    }

    void AgentInputController::Stop()
    {
        if (mActive)
            Input::EndSyntheticKeyboardSession();
        mHeldKeyNames.clear();
        mActive = false;
    }

    Json AgentInputController::ListKeys() const
    {
        Json names = Json::array();
        for (const KeyBinding& binding : KeyBindings)
            names.push_back(binding.Name);
        Json result = BuildStatus();
        result["keys"] = std::move(names);
        result["maxHeldKeys"] = MaxHeldKeys;
        return result;
    }

    Json AgentInputController::SetHeldKeys(const Json& params)
    {
        for (const auto& [name, value] : params.items())
        {
            (void)value;
            if (name != "held")
                throw std::invalid_argument("input.set contains an unknown parameter.");
        }
        if (!mActive)
            throw std::runtime_error("Synthetic keyboard session is not active.");
        if (!params.contains("held") || !params["held"].is_array() ||
            params["held"].size() > MaxHeldKeys)
        {
            throw std::invalid_argument("input.set held must be an array with at most 32 entries.");
        }

        std::vector<std::string> replacementNames;
        std::vector<KeyCode> replacementCodes;
        replacementNames.reserve(params["held"].size());
        replacementCodes.reserve(params["held"].size());
        for (const Json& value : params["held"])
        {
            if (!value.is_string())
                throw std::invalid_argument("input.set held entries must be key-name strings.");
            const std::string name = value.get<std::string>();
            const KeyBinding* binding = FindKey(name);
            if (!binding)
                throw std::invalid_argument("input.set contains an unknown key name.");
            if (std::find(replacementNames.begin(), replacementNames.end(), name) !=
                replacementNames.end())
            {
                throw std::invalid_argument("input.set held must not contain duplicates.");
            }
            replacementNames.push_back(name);
            replacementCodes.push_back(binding->Code);
        }

        if (!Input::SetSyntheticKeys(replacementCodes))
            throw std::runtime_error("Failed to replace synthetic keyboard state.");
        mHeldKeyNames = std::move(replacementNames);
        return BuildStatus();
    }

    Json AgentInputController::Clear(const Json& params)
    {
        RequireNoParams(params, "input.clear");
        if (!mActive)
            throw std::runtime_error("Synthetic keyboard session is not active.");
        Input::ClearSyntheticKeys();
        mHeldKeyNames.clear();
        return BuildStatus();
    }

    Json AgentInputController::Click(
        const Json& params,
        std::uint32_t viewportWidth,
        std::uint32_t viewportHeight)
    {
        for (const auto& [name, value] : params.items())
        {
            (void)value;
            if (name != "x" && name != "y" && name != "button")
                throw std::invalid_argument("input.click contains an unknown parameter.");
        }
        if (!mActive)
            throw std::runtime_error("Synthetic input session is not active.");
        if (!params.contains("x") || !params["x"].is_number() ||
            !params.contains("y") || !params["y"].is_number())
        {
            throw std::invalid_argument("input.click requires numeric x and y coordinates.");
        }

        const double x = params["x"].get<double>();
        const double y = params["y"].get<double>();
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || y < 0.0 ||
            x >= viewportWidth || y >= viewportHeight)
        {
            throw std::invalid_argument("input.click coordinates must be inside the main framebuffer.");
        }

        std::string buttonName = "left";
        if (params.contains("button"))
        {
            if (!params["button"].is_string())
                throw std::invalid_argument("input.click button must be left or right.");
            buttonName = params["button"].get<std::string>();
        }
        MouseCode button = MouseCode::ButtonLeft;
        if (buttonName == "right")
            button = MouseCode::ButtonRight;
        else if (buttonName != "left")
            throw std::invalid_argument("input.click button must be left or right.");

        const glm::vec2 position{ static_cast<float>(x), static_cast<float>(y) };
        Input::SetSyntheticPointerPosition(position);
        struct PointerOverrideGuard
        {
            ~PointerOverrideGuard() { Input::ClearSyntheticPointerPosition(); }
        } pointerOverrideGuard;

        MouseMovedEvent moved(position.x, position.y);
        Application::GetInstance().OnEvent(moved);
        MouseButtonPressedEvent pressed(button);
        Application::GetInstance().OnEvent(pressed);
        MouseButtonReleasedEvent released(button);
        Application::GetInstance().OnEvent(released);
        ++mVirtualClickCount;
        return Json{
            { "dispatched", true },
            { "mode", "virtual-event-click" },
            { "x", x },
            { "y", y },
            { "button", buttonName },
            { "clickCount", mVirtualClickCount }
        };
    }

    Json AgentInputController::BuildStatus() const
    {
        return Json{
            { "active", mActive },
            { "physicalKeyboardIsolated", mActive },
            { "physicalInputEventsIsolated", mActive },
            { "held", mHeldKeyNames },
            { "virtualPointerMode", "event-click-with-transient-position" },
            { "virtualClickCount", mVirtualClickCount }
        };
    }
}
