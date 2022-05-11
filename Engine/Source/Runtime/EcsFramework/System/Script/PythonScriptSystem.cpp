#include "hepch.h"

#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"

#include <pybind11/embed.h>

namespace py = pybind11;

namespace HEngine
{
    PythonScriptSystem::PythonScriptSystem(Level* level)
        : System(level)
    {
        if (std::getenv("PYTHONPATH") && std::getenv("PYTHONHOME"))
        {
            bLoadPython = true;
        }
    }

    void PythonScriptSystem::OnUpdateRuntime(Timestep ts)
    {
        if (!bLoadPython)
        {
            return;
        }
        py::scoped_interpreter guard{};
        auto testModule = py::module::import("Scripts.test");
        auto func = testModule.attr("OnUpdateRuntime");
        func();
    }

    void PythonScriptSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        if (!bLoadPython)
        {
            return;
        }
        py::scoped_interpreter guard{};
        auto testModule = py::module::import("Scripts.test");
        auto func = testModule.attr("OnUpdateEditor");
        func();
    }
}