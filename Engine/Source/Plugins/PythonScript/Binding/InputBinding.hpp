#pragma once

#include "Runtime/Core/Input/Input.h"

#include <pybind11/embed.h>

namespace py = pybind11;

namespace HEngine
{
    static void InputBinding(py::module_& HEngineModule)
    {
        py::module_ m_gamepadButton = HEngineModule.def_submodule("GamepadButton");
        py::enum_<GamepadButton>(m_gamepadButton, "GamepadButton")
            .value("A", GamepadButton::A)
            .value("B", GamepadButton::B)
            .value("X", GamepadButton::X)
            .value("Y", GamepadButton::Y)
            .value("LeftBumper", GamepadButton::LeftBumper)
            .value("RightBumper", GamepadButton::RightBumper)
            .value("Back", GamepadButton::Back)
            .value("Start", GamepadButton::Start)
            .value("Guide", GamepadButton::Guide)
            .value("LeftThumb", GamepadButton::LeftThumb)
            .value("RightThumb", GamepadButton::RightThumb)
            .value("DPadUp", GamepadButton::DPadUp)
            .value("DPadRight", GamepadButton::DPadRight)
            .value("DPadDown", GamepadButton::DPadDown)
            .value("DPadLeft", GamepadButton::DPadLeft)
            .export_values();

        py::module_ m_gamepadAxis = HEngineModule.def_submodule("GamepadAxis");
        py::enum_<GamepadAxis>(m_gamepadAxis, "GamepadAxis")
            .value("LeftX", GamepadAxis::LeftX)
            .value("LeftY", GamepadAxis::LeftY)
            .value("RightX", GamepadAxis::RightX)
            .value("RightY", GamepadAxis::RightY)
            .value("LeftTrigger", GamepadAxis::LeftTrigger)
            .value("RightTrigger", GamepadAxis::RightTrigger)
            .export_values();

        py::class_<GamepadState>(HEngineModule, "GamepadState")
            .def_property_readonly("Connected", [](const GamepadState& state)
            {
                return state.bConnected;
            })
            .def("IsButtonPressed", &GamepadState::IsButtonPressed)
            .def("GetAxis", &GamepadState::GetAxis);

        py::module_ m_keycode = HEngineModule.def_submodule("KeyCode");
        py::enum_<KeyCode>(m_keycode, "KeyCode")
            .value("Space", KeyCode::Space)
            .value("Apostrophe", KeyCode::Apostrophe)
            .value("Comma", KeyCode::Comma)
            .value("Minus", KeyCode::Minus)
            .value("Period", KeyCode::Period)
            .value("Slash", KeyCode::Slash)
            .value("D0", KeyCode::D0)
            .value("D1", KeyCode::D1)
            .value("D2", KeyCode::D2)
            .value("D3", KeyCode::D3)
            .value("D4", KeyCode::D4)
            .value("D5", KeyCode::D5)
            .value("D6", KeyCode::D6)
            .value("D7", KeyCode::D7)
            .value("D8", KeyCode::D8)
            .value("D9", KeyCode::D9)
            .value("Semicolon", KeyCode::Semicolon)
            .value("Equal", KeyCode::Equal)
            .value("A", KeyCode::A)
            .value("B", KeyCode::B)
            .value("C", KeyCode::C)
            .value("D", KeyCode::D)
            .value("E", KeyCode::E)
            .value("F", KeyCode::F)
            .value("G", KeyCode::G)
            .value("H", KeyCode::H)
            .value("I", KeyCode::I)
            .value("J", KeyCode::J)
            .value("K", KeyCode::K)
            .value("L", KeyCode::L)
            .value("M", KeyCode::M)
            .value("N", KeyCode::N)
            .value("O", KeyCode::O)
            .value("P", KeyCode::P)
            .value("Q", KeyCode::Q)
            .value("R", KeyCode::R)
            .value("S", KeyCode::S)
            .value("T", KeyCode::T)
            .value("U", KeyCode::U)
            .value("V", KeyCode::V)
            .value("W", KeyCode::W)
            .value("X", KeyCode::X)
            .value("Y", KeyCode::Y)
            .value("Z", KeyCode::Z)
            .value("LeftBracket", KeyCode::LeftBracket)
            .value("Backslash", KeyCode::Backslash)
            .value("RightBracket", KeyCode::RightBracket)
            .value("GraveAccent", KeyCode::GraveAccent)
            .value("World1", KeyCode::World1)
            .value("World2", KeyCode::World2)

            .value("Escape", KeyCode::Escape)
            .value("Enter", KeyCode::Enter)
            .value("Tab", KeyCode::Tab)
            .value("Backspace", KeyCode::Backspace)
            .value("Insert", KeyCode::Insert)
            .value("Delete", KeyCode::Delete)
            .value("Right", KeyCode::Right)
            .value("Left", KeyCode::Left)
            .value("Down", KeyCode::Down)
            .value("Up", KeyCode::Up)
            .value("PageUp", KeyCode::PageUp)
            .value("PageDown", KeyCode::PageDown)
            .value("Home", KeyCode::Home)
            .value("End", KeyCode::End)
            .value("CapsLock", KeyCode::CapsLock)
            .value("ScrollLock", KeyCode::ScrollLock)
            .value("NumLock", KeyCode::NumLock)
            .value("PrintScreen", KeyCode::PrintScreen)
            .value("Pause", KeyCode::Pause)
            .value("F1", KeyCode::F1)
            .value("F2", KeyCode::F2)
            .value("F3", KeyCode::F3)
            .value("F4", KeyCode::F4)
            .value("F5", KeyCode::F5)
            .value("F6", KeyCode::F6)
            .value("F7", KeyCode::F7)
            .value("F8", KeyCode::F8)
            .value("F9", KeyCode::F9)
            .value("F10", KeyCode::F10)
            .value("F11", KeyCode::F11)
            .value("F12", KeyCode::F12)
            .value("F13", KeyCode::F13)
            .value("F14", KeyCode::F14)
            .value("F15", KeyCode::F15)
            .value("F16", KeyCode::F16)
            .value("F17", KeyCode::F17)
            .value("F18", KeyCode::F18)
            .value("F19", KeyCode::F19)
            .value("F20", KeyCode::F20)
            .value("F21", KeyCode::F21)
            .value("F22", KeyCode::F22)
            .value("F23", KeyCode::F23)
            .value("F24", KeyCode::F24)
            .value("F25", KeyCode::F25)

            .value("KP0", KeyCode::KP0)
            .value("KP1", KeyCode::KP1)
            .value("KP2", KeyCode::KP2)
            .value("KP3", KeyCode::KP3)
            .value("KP4", KeyCode::KP4)
            .value("KP5", KeyCode::KP5)
            .value("KP6", KeyCode::KP6)
            .value("KP7", KeyCode::KP7)
            .value("KP8", KeyCode::KP8)
            .value("KP9", KeyCode::KP9)
            .value("KPDecimal", KeyCode::KPDecimal)
            .value("KPDivide", KeyCode::KPDivide)
            .value("KPMultiply", KeyCode::KPMultiply)
            .value("KPSubtract", KeyCode::KPSubtract)
            .value("KPAdd", KeyCode::KPAdd)
            .value("KPEnter", KeyCode::KPEnter)
            .value("KPEqual", KeyCode::KPEqual)

            .value("LeftShift", KeyCode::LeftShift)
            .value("LeftControl", KeyCode::LeftControl)
            .value("LeftAlt", KeyCode::LeftAlt)
            .value("LeftSuper", KeyCode::LeftSuper)
            .value("RightShift", KeyCode::RightShift)
            .value("RightControl", KeyCode::RightControl)
            .value("RightAlt", KeyCode::RightAlt)
            .value("RightSuper", KeyCode::RightSuper)
            .value("Menu", KeyCode::Menu)
            .export_values();

        py::module_ m_mousecode = HEngineModule.def_submodule("MouseCode");
        py::enum_<MouseCode>(m_mousecode, "MouseCode")
            .value("Button0", MouseCode::Button0)
            .value("Button1", MouseCode::Button1)
            .value("Button2", MouseCode::Button2)
            .value("Button3", MouseCode::Button3)
            .value("Button4", MouseCode::Button4)
            .value("Button5", MouseCode::Button5)
            .value("Button6", MouseCode::Button6)
            .value("Button7", MouseCode::Button7)
            .value("ButtonLeft", MouseCode::ButtonLeft)
            .value("ButtonRight", MouseCode::ButtonRight)
            .value("ButtonMiddle", MouseCode::ButtonMiddle)
            .export_values();

        // should defined after KeyCode to gen stub
        py::module_ InputModule = HEngineModule.def_submodule("Input");
        InputModule.def("IsKeyPressed", &Input::IsKeyPressed);
        InputModule.def("IsMouseButtonPressed", &Input::IsMouseButtonPressed);
        InputModule.def(
            "GetGamepadState", &Input::GetGamepadState,
            py::arg("gamepadIndex") = 0);
        InputModule.def("FindFirstConnectedGamepad", &Input::FindFirstConnectedGamepad);
    }
}
