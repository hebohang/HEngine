#include "Hepch.h"

#include "InputActions.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Events/KeyEvent.h"
#include "Runtime/Events/MouseEvent.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "_generated/serializer/InputMap.gen.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        float ReadBinding(const InputBindingRes& binding, const InputActionSnapshot& snapshot)
        {
            if (binding.Device == "Key") return snapshot.Keys[binding.Code] ? binding.Scale : 0.0f;
            if (binding.Device == "MouseButton") return snapshot.MouseButtons[binding.Code] ? binding.Scale : 0.0f;
            if (binding.Device == "PointerX") return snapshot.PointerDelta.x * binding.Scale;
            if (binding.Device == "PointerY") return snapshot.PointerDelta.y * binding.Scale;
            if (!snapshot.Gamepad.bConnected) return 0.0f;
            if (binding.Device == "GamepadButton") return snapshot.Gamepad.Buttons[binding.Code] ? binding.Scale : 0.0f;
            const float value = snapshot.Gamepad.Axes[binding.Code];
            return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) * binding.Scale : 0.0f;
        }

        glm::vec2 ApplyDeadZone(glm::vec2 value, float deadZone, bool bAnalog)
        {
            const float length = glm::length(value);
            if (!std::isfinite(length) || length <= deadZone) return glm::vec2(0);
            if (bAnalog)
                return value * ((std::min(length, 1.0f) - deadZone) / ((1.0f - deadZone) * length));
            return length > 1 ? value / length : value;
        }
    }

    InputActions::InputActions() { Configure(Defaults()); }
    InputActions& InputActions::Get() { static InputActions instance; return instance; }

    InputMapRes InputActions::Defaults()
    {
        InputMapRes map;
        auto add = [&](const char* name, const char* context, const char* type, std::vector<InputBindingRes> bindings)
        {
            InputActionRes action;
            action.Name = name; action.Context = context; action.Type = type; action.Bindings = std::move(bindings);
            map.Actions.push_back(std::move(action));
        };
        add("Move", "Gameplay", "Vector2", {{"Key",65,0,-1}, {"Key",68,0,1}, {"Key",87,1,1}, {"Key",83,1,-1},
            {"GamepadAxis",0,0,1}, {"GamepadAxis",1,1,-1}});
        add("Look", "Gameplay", "Vector2", {{"GamepadAxis",2,0,1}, {"GamepadAxis",3,1,-1}});
        add("Jump", "Gameplay", "Button", {{"Key",32,0,1}, {"GamepadButton",0,0,1}});
        add("Sprint", "Gameplay", "Button", {{"Key",340,0,1}, {"Key",344,0,1}, {"GamepadButton",9,0,1}});
        add("Crouch", "Gameplay", "Button", {{"Key",341,0,1}, {"GamepadButton",1,0,1}});
        add("Interact", "Gameplay", "Button", {{"Key",69,0,1}, {"GamepadButton",2,0,1}});
        add("Pause", "Gameplay", "Button", {{"Key",256,0,1}, {"GamepadButton",7,0,1}});
        add("Navigate", "UI", "Vector2", {{"Key",263,0,-1}, {"Key",262,0,1}, {"Key",265,1,1}, {"Key",264,1,-1},
            {"GamepadAxis",0,0,1}, {"GamepadAxis",1,1,-1},
            {"GamepadButton",14,0,-1}, {"GamepadButton",12,0,1}, {"GamepadButton",11,1,1}, {"GamepadButton",13,1,-1}});
        add("Submit", "UI", "Button", {{"Key",257,0,1}, {"GamepadButton",0,0,1}});
        add("Cancel", "UI", "Button", {{"Key",256,0,1}, {"GamepadButton",1,0,1}});
        return map;
    }

    void InputActions::Validate(const InputMapRes& map)
    {
        if (map.Version != 1 || map.Actions.empty() || map.Actions.size() > 256)
            throw std::invalid_argument("Input map requires version 1 and 1..256 actions.");
        std::set<std::string> names;
        for (const auto& action : map.Actions)
        {
            if (action.Name.empty() || action.Name.size() > 128 || action.Context.empty() || action.Context.size() > 128 ||
                !names.insert(action.Name).second || action.Bindings.size() > 32)
                throw std::invalid_argument("Input actions require unique bounded names, contexts and at most 32 bindings.");
            if (action.Type != "Button" && action.Type != "Axis" && action.Type != "Vector2")
                throw std::invalid_argument("Input action type must be Button, Axis or Vector2.");
            if (!std::isfinite(action.DeadZone) || action.DeadZone < 0 || action.DeadZone >= 1 ||
                !std::isfinite(action.HoldSeconds) || action.HoldSeconds <= 0 || action.HoldSeconds > 60 ||
                !std::isfinite(action.DoubleTapSeconds) || action.DoubleTapSeconds <= 0 || action.DoubleTapSeconds > 5)
                throw std::invalid_argument("Input action thresholds are invalid.");
            for (const auto& binding : action.Bindings)
            {
                int limit = binding.Device == "Key" ? 349 : binding.Device == "MouseButton" ? 8 :
                    binding.Device == "GamepadButton" ? static_cast<int>(GamepadButton::Count) :
                    binding.Device == "GamepadAxis" ? static_cast<int>(GamepadAxis::Count) :
                    (binding.Device == "PointerX" || binding.Device == "PointerY") ? 1 : 0;
                if (binding.Code < 0 || binding.Code >= limit || binding.Axis < 0 || binding.Axis > (action.Type == "Vector2" ? 1 : 0) ||
                    !std::isfinite(binding.Scale) || std::abs(binding.Scale) > 100 || binding.Scale == 0)
                    throw std::invalid_argument("Input binding device, code, axis or scale is invalid.");
                if (binding.Device == "Key" && binding.Code < 32)
                    throw std::invalid_argument("Input key code is outside the supported keyboard range.");
            }
        }
    }

    void InputActions::Configure(InputMapRes map)
    {
        Validate(map);
        std::map<std::string, RuntimeState, std::less<>> states;
        for (const auto& action : map.Actions) states.emplace(action.Name, RuntimeState{});
        const bool retainsContext = std::any_of(map.Actions.begin(), map.Actions.end(),
            [&](const auto& action) { return action.Context == mContext; });
        if (!retainsContext) mContext = map.Actions.front().Context;
        mMap = std::move(map); mStates = std::move(states);
        mPointerValid = false; mVirtualActions.clear();
        mPendingKeyPresses.fill(0); mPendingMousePresses.fill(0);
    }

    void InputActions::Load(const std::filesystem::path& path)
    {
        if (std::filesystem::file_size(path) > 1024 * 1024) throw std::invalid_argument("Input map exceeds 1 MiB.");
        Configure(Utils::LoadJsonFile(path).get<InputMapRes>());
    }

    void InputActions::Save(const std::filesystem::path& path) const
    {
        Validate(mMap);
        Utils::SaveJsonToFile(Json(mMap), path);
    }

    std::string InputActions::ExportMap() const { return Json(mMap).dump(); }

    void InputActions::ImportMap(std::string_view json)
    {
        if (json.size() > 1024 * 1024) throw std::invalid_argument("Input map exceeds 1 MiB.");
        Configure(Json::parse(json).get<InputMapRes>());
        for (auto& [name, state] : mStates) state.SuppressUntilRelease = true;
    }

    void InputActions::Rebind(std::string_view action, size_t bindingIndex, InputBindingRes binding)
    {
        auto candidate = mMap;
        auto found = std::find_if(candidate.Actions.begin(), candidate.Actions.end(), [&](const auto& item) { return item.Name == action; });
        if (found == candidate.Actions.end() || bindingIndex > found->Bindings.size()) throw std::invalid_argument("Unknown action or binding index.");
        if (bindingIndex == found->Bindings.size()) found->Bindings.push_back(std::move(binding));
        else found->Bindings[bindingIndex] = std::move(binding);
        Configure(std::move(candidate));
        for (auto& [name, state] : mStates) state.SuppressUntilRelease = true;
    }

    void InputActions::SetContext(std::string context)
    {
        if (context == mContext) return;
        if (std::none_of(mMap.Actions.begin(), mMap.Actions.end(), [&](const auto& action) { return action.Context == context; }))
            throw std::invalid_argument("Unknown input context.");
        mContext = std::move(context);
        for (auto& [name, state] : mStates) { state = {}; state.SuppressUntilRelease = true; }
        mPointerValid = false;
        mPendingKeyPresses.fill(0); mPendingMousePresses.fill(0);
    }

    void InputActions::Reset()
    {
        for (auto& [name, state] : mStates) state = {};
        mVirtualActions.clear(); mPointerValid = false;
        mPendingKeyPresses.fill(0); mPendingMousePresses.fill(0);
    }

    void InputActions::Suspend()
    {
        Reset();
        for (auto& [name, state] : mStates) state.SuppressUntilRelease = true;
    }

    void InputActions::BeginSession(InputMapRes map)
    {
        Validate(map);
        mContext = "Gameplay";
        Configure(std::move(map));
        Suspend();
    }

    void InputActions::ReloadProjectMap()
    {
        const auto& config = ConfigManager::GetInstance();
        InputMapRes map = Defaults();
        if (config.IsInitialized())
        {
            mProject = config.GetRootFolder();
            const auto path = mProject / "Config/Input.json";
            if (std::filesystem::exists(path))
            {
                try
                {
                    InputActions candidate;
                    candidate.Load(path);
                    map = candidate.GetMap();
                }
                catch (const std::exception& error) { HE_CORE_ERROR("Input map rejected; retaining defaults: {0}", error.what()); }
            }
        }
        BeginSession(std::move(map));
    }

    InputActionState InputActions::Read(std::string_view name) const
    {
        const auto found = mStates.find(name);
        if (found == mStates.end()) throw std::invalid_argument("Unknown input action: " + std::string(name));
        return found->second.State;
    }

    InputActionState InputActions::ReadOrDefault(std::string_view name) const
    {
        const auto found = mStates.find(name);
        return found == mStates.end() ? InputActionState{} : found->second.State;
    }

    void InputActions::SetVirtualAction(std::string name, glm::vec2 value)
    {
        if (!mStates.contains(name) || !std::isfinite(value.x) || !std::isfinite(value.y))
            throw std::invalid_argument("Virtual input requires a known action and finite values.");
        mVirtualActions[std::move(name)] = glm::clamp(value, glm::vec2(-1), glm::vec2(1));
    }

    void InputActions::ObserveEvent(const Event& event)
    {
        if (event.Handled) return;
        if (event.GetEventType() == EventType::KeyPressed)
        {
            const auto& pressed = static_cast<const KeyPressedEvent&>(event);
            const size_t code = static_cast<size_t>(pressed.GetKeyCode());
            if (pressed.GetRepeatCount() == 0 && code < mPendingKeyPresses.size())
                mPendingKeyPresses[code] +=
                    mPendingKeyPresses[code] != std::numeric_limits<uint32_t>::max();
        }
        else if (event.GetEventType() == EventType::MouseButtonPressed)
        {
            const size_t code = static_cast<size_t>(
                static_cast<const MouseButtonPressedEvent&>(event).GetMouseButton());
            if (code < mPendingMousePresses.size())
                mPendingMousePresses[code] +=
                    mPendingMousePresses[code] != std::numeric_limits<uint32_t>::max();
        }
    }

    void InputActions::Update(const InputActionSnapshot& snapshot, double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 60) throw std::invalid_argument("Input delta must be finite and in [0,60].");
        for (const auto& action : mMap.Actions)
        {
            auto& runtime = mStates.at(action.Name);
            const bool wasHeld = runtime.State.Held;
            const bool bSuppressPress = runtime.SuppressUntilRelease ||
                action.Context != mContext;
            uint32_t buttonPulses = 0;
            if (action.Type == "Button")
                for (size_t index = 0; index < action.Bindings.size(); ++index)
                {
                    const auto& binding = action.Bindings[index];
                    const bool duplicate = std::any_of(
                        action.Bindings.begin(), action.Bindings.begin() + index,
                        [&binding](const InputBindingRes& previous)
                        {
                            return previous.Device == binding.Device &&
                                previous.Code == binding.Code;
                        });
                    if (duplicate) continue;
                    uint32_t presses = 0;
                    if (binding.Device == "Key")
                        presses = snapshot.KeyPresses[binding.Code];
                    else if (binding.Device == "MouseButton")
                        presses = snapshot.MouseButtonPresses[binding.Code];
                    buttonPulses += std::min(
                        presses, std::numeric_limits<uint32_t>::max() - buttonPulses);
                }
            runtime.SinceTap = std::min(1000000.0, runtime.SinceTap + seconds);
            glm::vec2 value{0};
            glm::vec2 gamepadValue{0};
            for (const auto& binding : action.Bindings)
            {
                // Filter stick drift before combining it with keyboard/D-pad
                // input, which would otherwise lift the noise past the dead zone.
                auto& target = action.Type != "Button" && binding.Device == "GamepadAxis" ? gamepadValue : value;
                target[binding.Axis] += ReadBinding(binding, snapshot);
            }
            if (const auto it = snapshot.VirtualActions.find(action.Name); it != snapshot.VirtualActions.end())
                if (std::isfinite(it->second.x) && std::isfinite(it->second.y)) value += it->second;
            if (action.Type != "Vector2") { value.y = 0; gamepadValue.y = 0; }
            value = ApplyDeadZone(value, action.DeadZone, action.Type != "Button");
            value += ApplyDeadZone(gamepadValue, action.DeadZone, true);
            const float length = glm::length(value);
            if (length > 1) value /= length;
            // No synthetic press when returning from UI while a control is held.
            if (runtime.SuppressUntilRelease)
            {
                if (glm::length(value) == 0) runtime.SuppressUntilRelease = false;
                value = glm::vec2(0);
            }
            if (action.Context != mContext) value = glm::vec2(0);
            const bool held = glm::length(value) > 0;
            InputActionState state;
            state.Value = value; state.Held = held;
            state.PressCount = bSuppressPress ? 0 : buttonPulses;
            if (state.PressCount == 0 && held && !wasHeld)
                state.PressCount = 1;
            state.Pressed = state.PressCount != 0;
            state.Released = (!held && wasHeld) ||
                (buttonPulses != 0 && !held && !bSuppressPress);
            if (state.Pressed)
            {
                state.DoubleTapped = runtime.SinceTap <= action.DoubleTapSeconds;
                runtime.SinceTap = 0; runtime.DownSeconds = 0; runtime.HoldFired = false;
            }
            if (held)
            {
                runtime.DownSeconds += seconds;
                state.HoldPerformed = !runtime.HoldFired && runtime.DownSeconds >= action.HoldSeconds;
                runtime.HoldFired |= state.HoldPerformed;
            }
            else { runtime.DownSeconds = 0; runtime.HoldFired = false; }
            runtime.State = state;
        }
    }

    void InputActions::UpdateFromDevices(double seconds, bool enabled)
    {
        const auto& config = ConfigManager::GetInstance();
        if (config.IsInitialized() && mProject != config.GetRootFolder())
            ReloadProjectMap();
        if (!enabled) { Suspend(); return; }
        InputActionSnapshot snapshot;
        if (enabled)
        {
            for (const auto& action : mMap.Actions)
                for (const auto& binding : action.Bindings)
                {
                    if (binding.Device == "Key") snapshot.Keys[binding.Code] = Input::IsKeyPressed(static_cast<KeyCode>(binding.Code));
                    else if (binding.Device == "MouseButton") snapshot.MouseButtons[binding.Code] = Input::IsMouseButtonPressed(static_cast<MouseCode>(binding.Code));
                }
            const auto gamepad = Input::FindFirstConnectedGamepad();
            if (gamepad >= 0) snapshot.Gamepad = Input::GetGamepadState(static_cast<uint32_t>(gamepad));
            const auto pointer = Input::GetMousePosition();
            if (mPointerValid) snapshot.PointerDelta = pointer - mPreviousPointer;
            mPreviousPointer = pointer; mPointerValid = true;
            snapshot.VirtualActions = mVirtualActions;
        }
        else { mPointerValid = false; mVirtualActions.clear(); }
        UpdateWithPendingEvents(std::move(snapshot), std::clamp(seconds, 0.0, 60.0));
    }

    void InputActions::UpdateWithPendingEvents(
        InputActionSnapshot snapshot, double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 60)
            throw std::invalid_argument("Input delta must be finite and in [0,60].");
        for (size_t code = 0; code < snapshot.KeyPresses.size(); ++code)
            snapshot.KeyPresses[code] += std::min(mPendingKeyPresses[code],
                std::numeric_limits<uint32_t>::max() - snapshot.KeyPresses[code]);
        for (size_t code = 0; code < snapshot.MouseButtonPresses.size(); ++code)
            snapshot.MouseButtonPresses[code] += std::min(mPendingMousePresses[code],
                std::numeric_limits<uint32_t>::max() - snapshot.MouseButtonPresses[code]);
        mPendingKeyPresses.fill(0);
        mPendingMousePresses.fill(0);
        Update(snapshot, seconds);
    }
}
