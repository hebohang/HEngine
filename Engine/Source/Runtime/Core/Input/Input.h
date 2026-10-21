#pragma once

#include "GamepadCodes.h"
#include "KeyCodes.h"
#include "MouseCodes.h"

#include <glm/glm.hpp>

#if defined(HE_DEV_AGENT_RUNTIME)
#include <span>
#endif

namespace HEngine
{
    class HENGINE_API Input
    {
    public:
#if defined(HE_DEV_AGENT_RUNTIME)
        enum class SyntheticKeyQuery
        {
            UsePhysical,
            Released,
            Pressed
        };
#endif

        static bool IsKeyPressed(KeyCode key);
        // Optional mobile controls. Returns false on hosts without touch UI.
        static bool SetTouchControlsEnabled(bool enabled);

        static bool IsMouseButtonPressed(MouseCode button);
        static bool IsMouseButtonReleased(MouseCode button);
        static glm::vec2 GetMousePosition();
        [[nodiscard]] static float GetMouseX();
        [[nodiscard]] static float GetMouseY();

        [[nodiscard]] static GamepadState GetGamepadState(std::uint32_t gamepadIndex = 0);
        [[nodiscard]] static std::int32_t FindFirstConnectedGamepad();

#if defined(HE_DEV_AGENT_RUNTIME)
        // Dev Agent Runtime only. This changes the query source; it never injects
        // input into the operating system.
        static void BeginSyntheticKeyboardSession(bool bIsolatePhysicalKeyboard = true);
        static void EndSyntheticKeyboardSession();
        [[nodiscard]] static bool SetSyntheticKeys(std::span<const KeyCode> heldKeys);
        static void ClearSyntheticKeys();
        [[nodiscard]] static SyntheticKeyQuery QuerySyntheticKey(KeyCode key);
        static void SetSyntheticPointerPosition(glm::vec2 position);
        static void SetSyntheticRightMouseButton(bool held);
        [[nodiscard]] static SyntheticKeyQuery QuerySyntheticMouseButton(MouseCode button);
        static void ClearSyntheticPointerPosition();
        [[nodiscard]] static bool TryGetSyntheticPointerPosition(glm::vec2& position);
        [[nodiscard]] static bool IsSyntheticInputSessionActive();
#endif
    };
}
