#pragma once

#include "Runtime/Core/Input/GamepadCodes.h"
#include "Runtime/Resource/ResType/InputMap.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace HEngine
{
    class Event;
    struct InputActionSnapshot
    {
        std::array<bool, 349> Keys{};
        std::array<uint32_t, 349> KeyPresses{};
        std::array<bool, 8> MouseButtons{};
        std::array<uint32_t, 8> MouseButtonPresses{};
        GamepadState Gamepad;
        glm::vec2 PointerDelta{0.0f};
        // Host controls provide semantic values; they never simulate OS input.
        std::map<std::string, glm::vec2> VirtualActions;
    };

    struct InputActionState
    {
        glm::vec2 Value{0.0f};
        bool Pressed = false;
        bool Released = false;
        bool Held = false;
        bool HoldPerformed = false;
        bool DoubleTapped = false;
        uint32_t PressCount = 0;
    };

    // Engine-thread owned. Pure Update(snapshot) also supports headless tests/replay.
    // Exactly one context is active. Switching suppresses already-held controls
    // until released, so dismissing a menu cannot also jump or activate gameplay.
    class InputActions
    {
    public:
        InputActions();
        static InputActions& Get();
        static InputMapRes Defaults();
        static void Validate(const InputMapRes& map);
        void Configure(InputMapRes map);
        void Load(const std::filesystem::path& path);
        void Save(const std::filesystem::path& path) const;
        [[nodiscard]] std::string ExportMap() const;
        void ImportMap(std::string_view json);
        void Rebind(std::string_view action, size_t bindingIndex, InputBindingRes binding);
        void SetContext(std::string context);
        void Reset();
        void BeginSession(InputMapRes map);
        void ReloadProjectMap();
        void Suspend();
        // Called after UI event dispatch; retains short button presses until
        // the next display-frame input snapshot.
        void ObserveEvent(const Event& event);
        void Update(const InputActionSnapshot& snapshot, double seconds);
        // Combine a device snapshot with unconsumed events captured since the
        // previous update. Also used by headless event-sequence tests.
        void UpdateWithPendingEvents(InputActionSnapshot snapshot, double seconds);
        void UpdateFromDevices(double seconds, bool enabled = true);
        void SetVirtualAction(std::string name, glm::vec2 value);
        [[nodiscard]] InputActionState Read(std::string_view name) const;
        // Optional actions (for example the built-in character's Jump) are neutral
        // when a project intentionally omits them from its own map.
        [[nodiscard]] InputActionState ReadOrDefault(std::string_view name) const;
        [[nodiscard]] const InputMapRes& GetMap() const { return mMap; }
        [[nodiscard]] const std::string& GetContext() const { return mContext; }

    private:
        struct RuntimeState
        {
            InputActionState State;
            double DownSeconds = 0;
            double SinceTap = 1000000;
            bool HoldFired = false;
            bool SuppressUntilRelease = false;
        };
        InputMapRes mMap;
        std::map<std::string, RuntimeState, std::less<>> mStates;
        std::string mContext = "Gameplay";
        std::filesystem::path mProject;
        glm::vec2 mPreviousPointer{0.0f};
        bool mPointerValid = false;
        std::array<uint32_t, 349> mPendingKeyPresses{};
        std::array<uint32_t, 8> mPendingMousePresses{};
        std::map<std::string, glm::vec2> mVirtualActions;
    };
}
