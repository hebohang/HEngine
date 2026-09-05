#pragma once

#include "AudioBinding.hpp"
#include "EcsBinding.hpp"
#include "InputBinding.hpp"
#include "LogBinding.hpp"
#include "RmlUIBinding.hpp"
#include "RuntimeResourceWarmupBinding.hpp"
#include "SettingsBinding.hpp"
#include "UIBinding.hpp"
#include "VFXBinding.hpp"

#if HE_ENABLE_GM
#include "GMBinding.hpp"
#endif

#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Scripting/PythonPath.h"

#include <pybind11/embed.h>
#include <glm/glm.hpp>

namespace py = pybind11;

namespace HEngine
{
    py::str PyGetFullPath(const std::string& relativePath)
    {
        return ToPythonPath(ConfigManager::GetFullPath(relativePath));
    }

    py::str PyGetSavedPath(const std::string& relativePath)
    {
        return ToPythonPath(ConfigManager::GetSavedPath(relativePath));
    }

    PYBIND11_EMBEDDED_MODULE(HEngine, HEngineModule)
    {
        py::class_<Timestep>(HEngineModule, "Timestep")
            .def(py::init<float>())
            .def("GetSeconds", &Timestep::GetSeconds);

        py::class_<glm::vec2>(HEngineModule, "vec2")
            .def(py::init<float, float>())
            .def_readwrite("x", &glm::vec2::x)
            .def_readwrite("y", &glm::vec2::y);

        py::class_<glm::vec3>(HEngineModule, "vec3")
            .def(py::init<float, float, float>())
            .def_readwrite("x", &glm::vec3::x)
            .def_readwrite("y", &glm::vec3::y)
            .def_readwrite("z", &glm::vec3::z);

        py::class_<glm::vec4>(HEngineModule, "vec4")
            .def(py::init<float, float, float, float>())
            .def_readwrite("x", &glm::vec4::x)
            .def_readwrite("y", &glm::vec4::y)
            .def_readwrite("z", &glm::vec4::z)
            .def_readwrite("w", &glm::vec4::w);

        py::class_<glm::mat4>(HEngineModule, "mat4")
            .def(py::init<float>());

        HEngineModule.def("GetFullPath", &PyGetFullPath);
        HEngineModule.def("GetSavedPath", &PyGetSavedPath);
        HEngineModule.def("SetRuntimePaused", [](bool bPaused)
        {
            if (Level* level = PythonScriptSystem::GetActiveLevel())
                level->SetRuntimePaused(bPaused);
        });
        HEngineModule.def("RequestRuntimeScene", [](const std::string& scenePath)
        {
            return Application::GetInstance().RequestRuntimeScene(scenePath);
        });

        AudioBinding(HEngineModule);
        EcsBinding(HEngineModule);
        InputBinding(HEngineModule);
        LogBinding(HEngineModule);
        RmlUIBinding(HEngineModule);
        RuntimeResourceWarmupBinding(HEngineModule);
        SettingsBinding(HEngineModule);
        UIBinding(HEngineModule);
        VFXBinding(HEngineModule);
#if HE_ENABLE_GM
        GMBinding(HEngineModule);
#endif
    }
}
