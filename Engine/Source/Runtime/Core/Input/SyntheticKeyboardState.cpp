#include "Hepch.h"
#include "Runtime/Core/Input/Input.h"

#include <bitset>
#include <mutex>

namespace HEngine
{
    namespace
    {
        constexpr std::size_t SyntheticKeyCapacity =
            static_cast<std::size_t>(KeyCode::Menu) + 1;

        struct SyntheticKeyboardState
        {
            std::mutex Mutex;
            std::bitset<SyntheticKeyCapacity> HeldKeys;
            glm::vec2 PointerPosition{ 0.0f, 0.0f };
            bool bRightMouseHeld = false;
            bool bActive = false;
            bool bIsolatePhysicalKeyboard = true;
            bool bPointerPositionOverridden = false;
        };

        SyntheticKeyboardState& GetSyntheticKeyboardState()
        {
            static SyntheticKeyboardState state;
            return state;
        }

        [[nodiscard]] bool TryGetKeyIndex(KeyCode key, std::size_t& index)
        {
            index = static_cast<std::size_t>(key);
            return index < SyntheticKeyCapacity;
        }
    }

    void Input::BeginSyntheticKeyboardSession(bool bIsolatePhysicalKeyboard)
    {
        SyntheticKeyboardState& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        state.HeldKeys.reset();
        state.bIsolatePhysicalKeyboard = bIsolatePhysicalKeyboard;
        state.bPointerPositionOverridden = false;
        state.bRightMouseHeld = false;
        state.bActive = true;
    }

    void Input::EndSyntheticKeyboardSession()
    {
        SyntheticKeyboardState& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        state.HeldKeys.reset();
        state.bActive = false;
        state.bIsolatePhysicalKeyboard = true;
        state.bPointerPositionOverridden = false;
        state.bRightMouseHeld = false;
    }

    bool Input::SetSyntheticKeys(std::span<const KeyCode> heldKeys)
    {
        std::bitset<SyntheticKeyCapacity> replacement;
        for (const KeyCode key : heldKeys)
        {
            std::size_t index = 0;
            if (!TryGetKeyIndex(key, index))
                return false;
            replacement.set(index);
        }

        SyntheticKeyboardState& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        if (!state.bActive)
            return false;
        state.HeldKeys = replacement;
        return true;
    }

    void Input::ClearSyntheticKeys()
    {
        SyntheticKeyboardState& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        if (state.bActive)
            state.HeldKeys.reset();
    }

    Input::SyntheticKeyQuery Input::QuerySyntheticKey(KeyCode key)
    {
        std::size_t index = 0;
        if (!TryGetKeyIndex(key, index))
            return SyntheticKeyQuery::UsePhysical;

        SyntheticKeyboardState& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        if (!state.bActive)
            return SyntheticKeyQuery::UsePhysical;
        if (state.HeldKeys.test(index))
            return SyntheticKeyQuery::Pressed;
        return state.bIsolatePhysicalKeyboard
            ? SyntheticKeyQuery::Released
            : SyntheticKeyQuery::UsePhysical;
    }

    void Input::SetSyntheticRightMouseButton(bool held)
    {
        auto& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        if (state.bActive) state.bRightMouseHeld = held;
    }

    Input::SyntheticKeyQuery Input::QuerySyntheticMouseButton(MouseCode button)
    {
        auto& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        if (!state.bActive) return SyntheticKeyQuery::UsePhysical;
        return button == MouseCode::ButtonRight && state.bRightMouseHeld
            ? SyntheticKeyQuery::Pressed : SyntheticKeyQuery::Released;
    }

    void Input::SetSyntheticPointerPosition(glm::vec2 position)
    {
        SyntheticKeyboardState& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        if (!state.bActive)
            return;
        state.PointerPosition = position;
        state.bPointerPositionOverridden = true;
    }

    void Input::ClearSyntheticPointerPosition()
    {
        SyntheticKeyboardState& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        state.bPointerPositionOverridden = false;
        state.bRightMouseHeld = false;
    }

    bool Input::TryGetSyntheticPointerPosition(glm::vec2& position)
    {
        SyntheticKeyboardState& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        if (!state.bActive || !state.bPointerPositionOverridden)
            return false;
        position = state.PointerPosition;
        return true;
    }

    bool Input::IsSyntheticInputSessionActive()
    {
        SyntheticKeyboardState& state = GetSyntheticKeyboardState();
        std::scoped_lock lock(state.Mutex);
        return state.bActive;
    }
}
