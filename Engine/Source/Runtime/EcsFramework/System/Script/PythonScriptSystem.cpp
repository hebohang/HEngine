#include "hepch.h"

#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"

#include <pybind11/embed.h>

namespace HEngine
{
    void PythonScriptSystem::OnUpdateRuntime(Timestep ts)
    {
        if (!std::getenv("PYTHONPATH") || !std::getenv("PYTHONHOME"))
        {
            return;
        }
        pybind11::scoped_interpreter guard{};
        pybind11::print("hello, world!");
    }
}