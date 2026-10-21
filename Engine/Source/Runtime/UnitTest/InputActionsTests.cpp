#include "Hepch.h"

#include "Runtime/Core/Input/InputActions.h"
#include "Runtime/EcsFramework/System/Movement/CharacterInput.h"
#include "Runtime/Events/KeyEvent.h"
#include "Runtime/Events/MouseEvent.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace HEngine::Tests
{
    void InputActionsContract()
    {
        auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        InputActions input;
        InputActionSnapshot snapshot;
        snapshot.Keys[87] = true; snapshot.Keys[68] = true;
        input.Update(snapshot, 0.01);
        require(std::abs(glm::length(input.Read("Move").Value) - 1.0f) < 0.0001f, "Diagonal movement must be normalized.");
        snapshot.Keys[32] = true;
        input.Update(snapshot, 0.01);
        require(input.Read("Jump").Pressed && input.Read("Jump").Held, "Button press must be emitted once.");
        input.Update(snapshot, 0.4);
        require(!input.Read("Jump").Pressed && input.Read("Jump").HoldPerformed, "Hold threshold must fire.");
        input.Update(snapshot, 0.01);
        require(!input.Read("Jump").HoldPerformed, "Hold may not repeat each frame.");
        snapshot.Keys[32] = false;
        input.Update(snapshot, 0.01);
        require(input.Read("Jump").Released, "Release edge was lost.");
        input.Reset(); snapshot = {}; snapshot.Keys[32] = true;
        input.Update(snapshot, 0.01);
        snapshot.Keys[32] = false; input.Update(snapshot, 0.05);
        snapshot.Keys[32] = true; input.Update(snapshot, 0.05);
        require(input.Read("Jump").DoubleTapped, "Double tap interval was not recognized.");

        input.SetContext("UI");
        input.Update(snapshot, 0.01);
        require(!input.Read("Jump").Held, "Gameplay leaked through UI context.");
        input.SetContext("Gameplay"); input.Update(snapshot, 0.01);
        require(!input.Read("Jump").Held, "Held control leaked across context transition.");
        snapshot.Keys[32] = false; input.Update(snapshot, 0.01);
        snapshot.Keys[32] = true; input.Update(snapshot, 0.01);
        require(input.Read("Jump").Pressed, "Released control must become usable again.");

        input.Reset(); snapshot = {}; snapshot.Gamepad.bConnected = true;
        snapshot.Gamepad.Axes[0] = 0.05f; input.Update(snapshot, 0.01);
        require(!input.Read("Move").Held, "Stick noise bypassed dead zone.");
        snapshot.Keys[87] = true; input.Update(snapshot, 0.01);
        require(input.Read("Move").Value == glm::vec2(0, 1), "Stick drift deflected keyboard movement.");
        snapshot.Keys[87] = false;
        snapshot.Gamepad.Axes[0] = 0.6f; input.Update(snapshot, 0.01);
        require(std::abs(input.Read("Move").Value.x - (0.6f - 0.15f) / 0.85f) < 0.0001f,
            "Analog magnitude must be remapped continuously outside the dead zone.");
        snapshot.Gamepad.bConnected = false; input.Update(snapshot, 0.01);
        require(input.Read("Move").Released, "Disconnected gamepad left action stuck.");
        snapshot = {}; snapshot.VirtualActions["Move"] = {0.3f, 0.4f}; input.Update(snapshot, 0.01);
        require(std::abs(input.Read("Move").Value.y - 0.8f * (0.5f - 0.15f) / 0.85f) < 0.0001f,
            "Touch/virtual input was not composed.");

        // End-to-end action -> camera-relative velocity: light tilt must never
        // turn into full walking speed, even diagonally or with sprint enabled.
        snapshot = {}; snapshot.Gamepad.bConnected = true;
        const glm::vec3 right(0, 0, -1), forward(1, 0, 0);
        float previousSpeed = 0;
        for (float tilt : {0.0f, 0.14f, 0.15f, 0.151f, 0.3f, 0.6f, 1.0f})
        {
            snapshot.Gamepad.Axes[0] = tilt;
            input.Update(snapshot, 0.01);
            const auto velocity = CharacterInputVelocity(input.Read("Move").Value, right, forward, 4.0f);
            const float expected = std::max(0.0f, (tilt - 0.15f) / 0.85f) * 4.0f;
            require(std::abs(glm::length(velocity) - expected) < 0.0001f && glm::length(velocity) >= previousSpeed,
                "Stick strength must continuously control walking speed.");
            require(std::abs(velocity.z + expected) < 0.0001f, "Movement lost its camera-relative direction.");
            previousSpeed = glm::length(velocity);
        }
        snapshot.Gamepad.Axes[0] = 0.3f; snapshot.Gamepad.Axes[1] = -0.4f;
        input.Update(snapshot, 0.01);
        const auto walk = CharacterInputVelocity(input.Read("Move").Value, right, forward, 4.0f);
        const auto sprint = CharacterInputVelocity(input.Read("Move").Value, right, forward, 8.0f);
        require(std::abs(glm::length(walk) - 4.0f * 0.35f / 0.85f) < 0.0001f &&
            glm::length(sprint - walk * 2.0f) < 0.0001f, "Diagonal/sprint movement lost analog strength.");
        require(std::abs(glm::length(CharacterInputVelocity({1, 1}, right, forward, 4)) - 4) < 0.0001f,
            "Keyboard diagonal movement exceeded normal speed.");
        snapshot.Gamepad.Axes[0] = std::numeric_limits<float>::quiet_NaN(); snapshot.Gamepad.Axes[1] = 0;
        input.Update(snapshot, 0.01);
        require(!input.Read("Move").Held, "Invalid device input produced motion.");
        snapshot.Gamepad.Axes[2] = 1; snapshot.Gamepad.Axes[3] = -1;
        input.Update(snapshot, 0.01);
        require(input.Read("Look").Value.x > 0 && input.Read("Look").Value.y > 0 &&
            std::abs(glm::length(input.Read("Look").Value) - 1) < 0.0001f, "Right-stick look axes were lost.");
        input.SetContext("UI"); input.Update({}, 0.01);
        snapshot = {}; snapshot.Gamepad.bConnected = true;
        snapshot.Gamepad.Buttons[static_cast<size_t>(GamepadButton::DPadDown)] = true;
        snapshot.Gamepad.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
        input.Update(snapshot, 0.01);
        require(input.Read("Navigate").Value.y == -1 && input.Read("Submit").Pressed &&
            !input.Read("Jump").Held && !input.Read("Look").Held, "Gamepad UI controls leaked into gameplay.");
        input.SetContext("Gameplay"); input.Update(snapshot, 0.01);
        require(!input.Read("Jump").Held, "UI confirmation triggered a jump on resume.");
        input.Update({}, 0.01); input.Update(snapshot, 0.01);
        require(input.Read("Jump").Pressed, "Gamepad button failed to reactivate after release.");

        input.Rebind("Jump", 0, {"Key",70,0,1});
        snapshot = {}; input.Update(snapshot, 0.01);
        snapshot.Keys[32] = true; input.Update(snapshot, 0.01);
        require(!input.Read("Jump").Held, "Old key remains bound after remapping.");
        snapshot.Keys[70] = true; input.Update(snapshot, 0.01);
        require(input.Read("Jump").Pressed, "New key was not bound.");
        bool rejected = false;
        try { input.Rebind("Jump", 0, {"Key",999,0,1}); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && input.GetMap().Actions[2].Bindings[0].Code == 70, "Invalid candidate mutated live bindings.");
        auto invalid = input.GetMap(); invalid.Actions.push_back(invalid.Actions.front());
        rejected = false;
        try { input.Configure(invalid); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Duplicate action IDs were accepted.");

        const auto path = std::filesystem::temp_directory_path() /
            ("he-input-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        struct Cleanup { std::filesystem::path Path; ~Cleanup() { std::error_code error; std::filesystem::remove(Path, error); } } cleanup{path};
        input.Save(path);
        InputActions restored; restored.Load(path);
        snapshot = {}; snapshot.Keys[70] = true; restored.Update(snapshot, 0.01);
        require(restored.Read("Jump").Pressed, "Persisted remapping did not survive reload.");

        const auto exported = restored.ExportMap();
        restored.SetContext("UI");
        restored.BeginSession(input.GetMap());
        restored.Update(snapshot, 0.01);
        require(restored.GetContext() == "Gameplay" && !restored.Read("Jump").Held,
            "Play restart resets context and suppresses controls already held.");
        snapshot = {}; restored.Update(snapshot, 0.01);
        snapshot.Keys[70] = true; restored.Update(snapshot, 0.01);
        require(restored.Read("Jump").Pressed, "New session accepts a fresh press.");
        restored.Suspend(); restored.Update(snapshot, 0.01);
        require(!restored.Read("Jump").Held, "Focus restoration cannot synthesize a held press.");
        restored.ImportMap(exported);
        restored.Update(snapshot, 0.01);
        require(!restored.Read("Jump").Held, "Loading preferences must suppress a currently held key.");
        snapshot = {}; restored.Update(snapshot, 0.01);
        snapshot.Keys[70] = true; restored.Update(snapshot, 0.01);
        require(restored.Read("Jump").Pressed, "Loaded binding did not reactivate after release.");
        rejected = false;
        try { restored.ImportMap("{}"); } catch (const std::exception&) { rejected = true; }
        require(rejected && restored.ExportMap() == exported, "Invalid saved bindings changed the live map.");

        InputMapRes custom;
        InputActionRes customAction;
        customAction.Name = "Confirm"; customAction.Context = "Menu";
        customAction.Bindings = {{"Key",257,0,1}};
        custom.Actions = {customAction}; restored.Configure(custom);
        snapshot = {}; snapshot.Keys[257] = true; restored.Update(snapshot, 0.01);
        require(restored.GetContext() == "Menu" && restored.Read("Confirm").Pressed,
            "A map without Gameplay must activate its first context.");
        require(!restored.ReadOrDefault("Move").Held && restored.ReadOrDefault("Jump").Value == glm::vec2(0),
            "Omitted optional character actions must remain neutral.");

        InputActions shortPresses;
        KeyPressedEvent uiHandled(KeyCode::Space, 0);
        uiHandled.Handled = true;
        shortPresses.ObserveEvent(uiHandled);
        shortPresses.UpdateWithPendingEvents({}, 0.05);
        require(!shortPresses.Read("Jump").Pressed,
            "A UI-consumed key press reached gameplay.");
        KeyPressedEvent repeat(KeyCode::Space, 1);
        shortPresses.ObserveEvent(repeat);
        shortPresses.UpdateWithPendingEvents({}, 0.05);
        require(!shortPresses.Read("Jump").Pressed,
            "A key-repeat event created a new press edge.");
        KeyPressedEvent quickJump(KeyCode::Space, 0);
        KeyReleasedEvent quickRelease(KeyCode::Space);
        shortPresses.ObserveEvent(quickJump);
        shortPresses.ObserveEvent(quickRelease);
        shortPresses.UpdateWithPendingEvents({}, 0.05);
        require(shortPresses.Read("Jump").Pressed && shortPresses.Read("Jump").Released &&
            shortPresses.Read("Jump").PressCount == 1 &&
            !shortPresses.Read("Jump").Held,
            "A press and release between display frames lost the Jump edge.");
        shortPresses.UpdateWithPendingEvents({}, 0.05);
        require(!shortPresses.Read("Jump").Pressed,
            "A short Jump press was replayed on another display frame.");

        InputMapRes shootingMap = InputActions::Defaults();
        InputActionRes fire;
        fire.Name = "Fire";
        fire.Context = "Gameplay";
        fire.Type = "Button";
        fire.Bindings = {{"MouseButton", 0, 0, 1}, {"MouseButton", 0, 0, 1}, {"GamepadAxis", 5, 0, 1}};
        shootingMap.Actions.push_back(std::move(fire));
        shortPresses.Configure(std::move(shootingMap));
        MouseButtonPressedEvent quickFire(MouseCode::ButtonLeft);
        shortPresses.ObserveEvent(quickFire);
        shortPresses.ObserveEvent(quickFire);
        shortPresses.UpdateWithPendingEvents({}, 0.05);
        require(shortPresses.Read("Fire").Pressed && shortPresses.Read("Fire").Released &&
            shortPresses.Read("Fire").PressCount == 2 &&
            !shortPresses.Read("Fire").Held,
            "Two mouse fire commands were coalesced between display frames.");
        shortPresses.SetContext("UI");
        shortPresses.ObserveEvent(quickFire);
        shortPresses.UpdateWithPendingEvents({}, 0.05);
        require(!shortPresses.Read("Fire").Pressed,
            "A mouse fire command crossed into the UI context.");
        shortPresses.SetContext("Gameplay"); shortPresses.Update({}, 0.01);
        snapshot = {}; snapshot.Gamepad.bConnected = true;
        snapshot.Gamepad.Axes[5] = 0.1f; shortPresses.Update(snapshot, 0.01);
        require(!shortPresses.Read("Fire").Held, "A resting trigger fired below its threshold.");
        snapshot.Gamepad.Axes[5] = 0.6f; shortPresses.Update(snapshot, 0.01);
        require(shortPresses.Read("Fire").Pressed, "Trigger pull did not fire.");
        shortPresses.Update(snapshot, 0.01);
        require(shortPresses.Read("Fire").Held && !shortPresses.Read("Fire").Pressed,
            "Holding a trigger must not repeat press edges.");
        snapshot.Gamepad.bConnected = false; shortPresses.Update(snapshot, 0.01);
        require(shortPresses.Read("Fire").Released && !shortPresses.Read("Fire").Held,
            "Disconnecting a held trigger left fire stuck.");
    }
}
