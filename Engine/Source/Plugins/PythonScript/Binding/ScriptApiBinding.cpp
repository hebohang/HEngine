#include "ScriptApiBinding.h"
#include "Runtime/Scripting/ScriptEngineApi.h"
#include "Runtime/Scripting/ScriptCore.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace HEngine
{
    namespace py = pybind11;
    namespace
    {
        thread_local std::uint64_t sCurrentEntity = 0;
        using Op = ScriptAbi::EngineOperation;
        struct State { glm::vec3 Position{}, Rotation{}, Scale{1}; glm::vec4 Color{1}; bool HasSprite = false; };
        struct InputState { glm::vec2 Movement{}, Pointer{}; bool PointerDown = false; std::uint64_t PressSequence = 0; };
        ScriptAbi::Request Request(Op operation, std::uint64_t entity = 0)
        {
            ScriptAbi::Request r{};
            r.Version = ScriptAbi::Version; r.Operation = static_cast<std::uint32_t>(operation); r.Entity = entity;
            return r;
        }
        std::string Call(ScriptAbi::Request& r, const std::string& name = {}, const std::string& input = {})
        {
            if (name.size() > 4096 || input.size() > 1024 * 1024) throw std::invalid_argument("Python engine API buffer limit exceeded.");
            // Buffer is a synchronous borrow; no interpreter object crosses the ABI.
            thread_local std::vector<char> output(1024 * 1024);
            r.Name = reinterpret_cast<std::uintptr_t>(name.data()); r.NameLength = static_cast<std::int32_t>(name.size());
            r.Input = reinterpret_cast<std::uintptr_t>(input.data()); r.InputLength = static_cast<std::int32_t>(input.size());
            r.Output = reinterpret_cast<std::uintptr_t>(output.data()); r.OutputCapacity = static_cast<std::int32_t>(output.size());
            const auto status = ScriptEngineApi::Call(&r);
            if (r.OutputLength < 0 || r.OutputLength > r.OutputCapacity) throw std::runtime_error("Invalid engine API output length.");
            const std::string text(output.data(), r.OutputLength);
            if (status != 0) throw std::runtime_error(text.empty() ? "Engine script API failed." : text);
            return text;
        }
        State ReadState(PythonScriptEntity entity)
        {
            auto r = Request(Op::ReadEntity, entity.Handle); Call(r);
            return {{r.Data[0], r.Data[1], r.Data[2]}, {r.Data[3], r.Data[4], r.Data[5]},
                {r.Data[6], r.Data[7], r.Data[8]}, {r.Data[9], r.Data[10], r.Data[11], r.Data[12]}, (r.Flags & 2) != 0};
        }
        void WriteState(PythonScriptEntity entity, const State& state)
        {
            auto r = Request(Op::WriteEntity, entity.Handle); r.Flags = state.HasSprite ? 3 : 1;
            std::copy_n(&state.Position.x, 3, r.Data); std::copy_n(&state.Rotation.x, 3, r.Data + 3);
            std::copy_n(&state.Scale.x, 3, r.Data + 6); std::copy_n(&state.Color.x, 4, r.Data + 9); Call(r);
        }
    }
    void ScriptApiBinding(py::module_& module)
    {
        auto script = module.def_submodule("Script", "Portable engine API; main-thread, session-handle and copy semantics.");
        py::class_<ScriptContactEvent>(script, "Contact")
            .def_readonly("Phase", &ScriptContactEvent::Phase)
            .def_readonly("Point", &ScriptContactEvent::Point)
            .def_readonly("Normal", &ScriptContactEvent::Normal)
            .def_readonly("IsTrigger", &ScriptContactEvent::Trigger)
            .def_property_readonly("Other", [](const ScriptContactEvent& contact) { return PythonScriptEntity{contact.Other}; });
        py::class_<State>(script, "EntityState")
            .def(py::init<>()).def_readwrite("Position", &State::Position).def_readwrite("Rotation", &State::Rotation)
            .def_readwrite("Scale", &State::Scale).def_readwrite("Color", &State::Color).def_readwrite("HasSprite", &State::HasSprite);
        py::class_<InputState>(script, "InputState")
            .def_readonly("Movement", &InputState::Movement).def_readonly("Pointer", &InputState::Pointer)
            .def_readonly("PointerDown", &InputState::PointerDown).def_readonly("PressSequence", &InputState::PressSequence);
        py::class_<PythonScriptEntity>(script, "Entity")
            .def(py::init<>()).def_readonly("Handle", &PythonScriptEntity::Handle)
            .def_property_readonly("IsValid", [](PythonScriptEntity entity) { auto r = Request(Op::EntityValid, entity.Handle); Call(r); return r.Result != 0; })
            .def_property("State", &ReadState, &WriteState).def("Apply", &WriteState)
            .def("Destroy", [](PythonScriptEntity entity) { auto r = Request(Op::DestroyEntity, entity.Handle); Call(r); })
            .def("SetScriptEnabled", [](PythonScriptEntity entity, bool enabled) { auto r = Request(Op::SetEnabled, entity.Handle); r.Flags = enabled; Call(r); })
            .def("Find", [](PythonScriptEntity entity, const std::string& id) { auto r = Request(Op::FindEntity, entity.Handle); Call(r, id); return PythonScriptEntity{r.Result}; })
            .def_property("Texture", [](PythonScriptEntity entity) { auto r = Request(Op::ReadTexture, entity.Handle); return Call(r); },
                [](PythonScriptEntity entity, const std::string& path) { auto r = Request(Op::WriteTexture, entity.Handle); Call(r, path); })
            .def_property_readonly("Input", [](PythonScriptEntity entity) {
                auto r = Request(Op::ReadInput, entity.Handle); Call(r);
                return InputState{{r.Data[0], r.Data[1]}, {r.Data[2], r.Data[3]}, r.Flags != 0, r.Result};
            });
        script.def("Log", [](const std::string& message) { auto r = Request(Op::Log); Call(r, {}, message); });
        script.def("ReadSave", [](const std::string& key) { auto r = Request(Op::ReadSave); return Call(r, key); });
        script.def("WriteSave", [](const std::string& key, const std::string& value) { auto r = Request(Op::WriteSave); Call(r, key, value); });
        script.def("EmitEvent", [](const std::string& name, const std::string& payload) { auto r = Request(Op::EmitEvent, sCurrentEntity); Call(r, name, payload); });
        script.def("ReadEvent", [](const std::string& name, std::uint64_t after) {
            auto r = Request(Op::ReadEvent, sCurrentEntity); r.Result = after; auto text = Call(r, name); return py::make_tuple(r.Result, text);
        });
    }
    PythonScriptContext::PythonScriptContext(std::uint64_t entity) : mPrevious(sCurrentEntity) { sCurrentEntity = entity; }
    PythonScriptContext::~PythonScriptContext() { sCurrentEntity = mPrevious; }
}
