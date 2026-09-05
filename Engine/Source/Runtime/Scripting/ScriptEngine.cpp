#include "Hepch.h"

#include "ScriptEngine.h"
#include "Binding/CoreBinding.hpp"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Scripting/PythonPath.h"
#include "Runtime/Utils/StringUtils/StringUtils.h"

#if HE_ENABLE_GM
#include "Runtime/Scripting/Binding/GMBinding.hpp"
#endif

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace HEngine
{
    static std::unordered_map<UUID, py::module_> ModuleMap;
    static std::unordered_map<UUID, std::string> ClassNameMap;
    static std::unordered_map<UUID, py::object>  PyClassMap;
    static std::unordered_map<UUID, std::string> RegistrationErrorMap;

    static void ThrowIfPythonConfigFailed(PyConfig& config, PyStatus status, const char* fallbackMessage)
    {
        if (PyStatus_Exception(status) == 0)
            return;

        const std::string message = PyStatus_IsError(status) != 0 && status.err_msg != nullptr
            ? status.err_msg
            : fallbackMessage;
        PyConfig_Clear(&config);
        throw std::runtime_error(message);
    }

    static void HInitPyInterpreter(bool init_signal_handlers = true,
                                   int argc = 0,
                                   const char* const* argv = nullptr,
                                   bool add_program_dir_to_path = true)
    {
#if PY_VERSION_HEX < PYBIND11_PYCONFIG_SUPPORT_PY_VERSION_HEX
        detail::initialize_interpreter_pre_pyconfig(
            init_signal_handlers, argc, argv, add_program_dir_to_path);
#else
        PyConfig config;
        PyConfig_InitIsolatedConfig(&config);
#if defined(HE_PACKAGED_GAME)
        config.isolated = 1;
        config.use_environment = 0;
        config.site_import = 0;
        config.user_site_directory = 0;
        config.write_bytecode = 0;
        config.safe_path = 1;
#else
        config.isolated = 0;
        config.use_environment = 1;
#endif
        config.install_signal_handlers = init_signal_handlers ? 1 : 0;

        // to find python311.dll and the encodings module
        const std::wstring projectRootPath = ConfigManager::GetInstance().GetRootFolder().wstring();
        const std::wstring engineContentRootPath =
            ConfigManager::GetInstance().GetEngineContentFolder().wstring();
        const std::wstring projectScriptsPath =
            ConfigManager::GetProjectFullPath("Scripts").wstring();
        const std::wstring engineScriptsPath =
            ConfigManager::GetEngineContentFullPath("Scripts").wstring();
        const std::wstring scriptsLibPath =
            ConfigManager::GetEngineContentFullPath("Scripts/Lib").wstring();
        PyStatus status = PyConfig_SetString(&config, &config.prefix, engineScriptsPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the CPython prefix");
        status = PyConfig_SetString(&config, &config.exec_prefix, engineScriptsPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the CPython executable prefix");

#if defined(HE_PACKAGED_GAME)
        // Scene components can use package-qualified imports, so the cooked
        // Content root must be present alongside the script and standard-library
        // directories.
        status = PyWideStringList_Append(&config.module_search_paths, projectRootPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the packaged content path");
        status = PyWideStringList_Append(&config.module_search_paths, projectScriptsPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the packaged Python script path");
        status = PyWideStringList_Append(&config.module_search_paths, engineContentRootPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the packaged engine content path");
        status = PyWideStringList_Append(&config.module_search_paths, scriptsLibPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the packaged Python standard library path");
        config.module_search_paths_set = 1;
#endif

        py::detail::precheck_interpreter();
        status = PyConfig_SetBytesArgv(&config, argc, const_cast<char* const*>(argv));
        ThrowIfPythonConfigFailed(config, status, "Failed to prepare CPython");
        status = Py_InitializeFromConfig(&config);
        ThrowIfPythonConfigFailed(config, status, "Failed to initialize CPython");
        if (add_program_dir_to_path) {
            PyRun_SimpleString("import sys, os.path; "
                "sys.path.insert(0, "
                "os.path.abspath(os.path.dirname(sys.argv[0])) "
                "if sys.argv and os.path.exists(sys.argv[0]) else '')");
        }
        PyConfig_Clear(&config);
#endif
    }

    void ScriptEngine::Init()
    {
#if defined(HE_PACKAGED_GAME)
        HInitPyInterpreter(true, 0, nullptr, false);
#else
        HInitPyInterpreter();
        py::list pythonPath = py::module::import("sys").attr("path");
        pythonPath.attr("append")(ToPythonPath(ConfigManager::GetInstance().GetRootFolder()));
        pythonPath.attr("append")(ToPythonPath(ConfigManager::GetProjectFullPath("Scripts")));
        pythonPath.attr("append")(ToPythonPath(ConfigManager::GetInstance().GetEngineContentFolder()));
        pythonPath.attr("append")(ToPythonPath(ConfigManager::GetEngineContentFullPath("Scripts/Lib")));
        pythonPath.attr("append")(ToPythonPath(
            ConfigManager::GetEngineContentFullPath("Scripts/Lib/site-packages")));
#endif
#if HE_ENABLE_GM
        InitializeProjectGMCommands();
#endif
    }

    void ScriptEngine::Shutdown()
    {
        if (Py_IsInitialized() == 0)
            return;

        // All pybind11 owners must release their references while CPython is alive.
        // Keeping released handles in static maps leaves leaked extension objects for
        // Py_FinalizeEx and can crash inside python311.dll during process shutdown.
#if HE_ENABLE_GM
        ShutdownProjectGMCommands();
#endif
        PyClassMap.clear();
        ModuleMap.clear();
        ClassNameMap.clear();
        RegistrationErrorMap.clear();
        py::finalize_interpreter();
    }

    bool ScriptEngine::RegisterModule(const UUID& uuid, const std::string& scriptPath)
    {
        auto lastPointPos = scriptPath.find_last_of(".");
        std::string pyModuleName = scriptPath.substr(0, lastPointPos);
		RegistrationErrorMap.erase(uuid);
        try 
        {
            auto pyModule = py::module::import(pyModuleName.c_str());
            ModuleMap[uuid] = pyModule;
            auto secondLastPointPos = pyModuleName.find_last_of(".");
            if (secondLastPointPos != std::string::npos)
            {
                ClassNameMap[uuid] = pyModuleName.substr(secondLastPointPos + 1, -1);
            }
            else
            {
                ClassNameMap[uuid] = pyModuleName;
            }

            // init a python class and register in the Map
            PyClassMap[uuid] = pyModule.attr(ClassNameMap[uuid].c_str())();
            return true;
        }
        catch (py::error_already_set& e)
        {
            HE_CORE_ERROR(e.what());
			RegistrationErrorMap[uuid] = e.what();
            PyClassMap.erase(uuid);
            ClassNameMap.erase(uuid);
            ModuleMap.erase(uuid);
            return false;
        }
    }

    std::string ScriptEngine::GetLastRegistrationError(const UUID& uuid)
    {
        const auto error = RegistrationErrorMap.find(uuid);
        return error == RegistrationErrorMap.end() ? std::string{} : error->second;
    }

    bool ScriptEngine::IsModuleRegistered(const UUID& uuid)
    {
        return ModuleMap.find(uuid) != ModuleMap.end();
    }

    bool ScriptEngine::IsClassRegistered(const UUID& uuid)
    {
        return PyClassMap.find(uuid) != PyClassMap.end();
    }

    py::module_& ScriptEngine::GetPyModule(const UUID& uuid)
    {
        HE_CORE_ASSERT(ModuleMap.find(uuid) != ModuleMap.end() && "Can't find this member in Library!");
        return ModuleMap[uuid];
    }

    std::string ScriptEngine::GetPyClassName(const UUID& uuid)
    {
        HE_CORE_ASSERT(ClassNameMap.find(uuid) != ClassNameMap.end() && "Can't find this member in Library!");
        return ClassNameMap[uuid];
    }

    pybind11::object& ScriptEngine::GetPyClass(const UUID& uuid)
    {
        HE_CORE_ASSERT(PyClassMap .find(uuid) != PyClassMap.end() && "Can't find this member in Library!");
        return PyClassMap[uuid];
    }

    void ScriptEngine::ReloadModule(const UUID& uuid)
    {
        HE_CORE_ASSERT(ModuleMap.find(uuid) != ModuleMap.end() && "Can't find this member in Library!");
        ModuleMap[uuid].reload();
    }

    void ScriptEngine::ReloadModuleAndClass(const UUID& uuid)
    {
        HE_CORE_ASSERT(ModuleMap.find(uuid) != ModuleMap.end() && "Can't find this member in Library!");
        HE_CORE_ASSERT(PyClassMap.find(uuid) != PyClassMap.end() && "Can't find this member in Library!");
        // reload() modifies *this in-place via reinterpret_steal and returns void.
        ModuleMap[uuid].reload();
        // Re-instantiate the class from the freshly reloaded module.
        PyClassMap[uuid] = ModuleMap[uuid].attr(ClassNameMap[uuid].c_str())();
    }

    void ScriptEngine::UnregisterModule(const UUID& uuid)
    {
        auto modIt = ModuleMap.find(uuid);
        if (modIt != ModuleMap.end())
            ModuleMap.erase(modIt);

        ClassNameMap.erase(uuid);
        RegistrationErrorMap.erase(uuid);

        auto classIt = PyClassMap.find(uuid);
        if (classIt != PyClassMap.end())
            PyClassMap.erase(classIt);
    }

    std::string ScriptEngine::GetPyObjType(const py::handle& pyObj)
    {
        std::string typeStr = py::type::of(pyObj).str();
        auto firstPos = typeStr.find_first_of("'");
        auto lastPos = typeStr.find_last_of("'");
        HE_CORE_ASSERT(firstPos != std::string::npos);
        HE_CORE_ASSERT(lastPos != std::string::npos);
        return typeStr.substr(firstPos + 1, lastPos - firstPos - 1);
    }
}
