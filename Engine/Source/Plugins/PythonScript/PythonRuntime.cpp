#include "Hepch.h"

#include "PythonRuntime.h"
#include "PythonBackend.h"
#include "Plugins/PythonScript/Binding/ReflectedComponents.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Core/Timestep.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Core/UUID.h"
#include "Plugins/PythonScript/PythonPath.h"
#include "Runtime/Utils/StringUtils/StringUtils.h"
#include "Runtime/UI/Rml/RmlUiEventPayload.h"

#if HE_ENABLE_GM
#include "Plugins/PythonScript/Binding/GMBinding.hpp"
#endif

#if defined(__APPLE__)
#include "Runtime/Platform/PlatformPaths.h"
#include "PythonBuildConfig.h"
#endif

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <algorithm>
#include <stdexcept>

namespace py = pybind11;

namespace HEngine
{
    namespace
    {
        std::string ScriptModuleName(const std::string& scriptPath)
        {
            const auto invalid = [] { throw std::invalid_argument("Invalid Python script path: use a dotted module name or Scripts/<module>.py with identifier segments."); };
            if (scriptPath.empty() || scriptPath.size() > 512 || scriptPath.find('\0') != std::string::npos)
                invalid();
            std::string module = scriptPath;
            const bool resourcePath = module.find_first_of("/\\") != std::string::npos;
            std::replace(module.begin(), module.end(), '\\', '/');
            if (resourcePath && (!module.starts_with("Scripts/") || !module.ends_with(".py")))
                invalid();
            if (module.ends_with(".py")) module.resize(module.size() - 3);
            // A filename containing dots is not a package separator in a
            // resource path. Reject it instead of importing a different file.
            if (resourcePath && module.find('.') != std::string::npos) invalid();
            std::replace(module.begin(), module.end(), '/', '.');
            size_t start = 0;
            while (start <= module.size())
            {
                const size_t end = module.find('.', start);
                const std::string segment = module.substr(start, end == std::string::npos ? end : end - start);
                if (segment.empty() || PyUnicode_IsIdentifier(py::str(segment).ptr()) != 1) invalid();
                if (end == std::string::npos) break;
                start = end + 1;
            }
            return module;
        }
    }

    static std::unordered_map<UUID, py::module_> ModuleMap;
    static std::unordered_map<UUID, std::string> ClassNameMap;
    static std::unordered_map<UUID, py::object>  PyClassMap;
    static std::unordered_map<UUID, std::string> RegistrationErrorMap;

    void PythonRuntime::CheckThread() const
    {
        if (mThread != std::this_thread::get_id())
            throw std::logic_error("Python callbacks must run on the engine owning thread.");
    }

    template<typename F> void PythonRuntime::Guard(F&& function)
    {
        CheckThread();
        py::gil_scoped_acquire acquire;
        try { function(); }
        catch (py::error_already_set& error)
        {
            // Materialize traceback before releasing the GIL. A pybind exception
            // must never survive beyond this private runtime adapter.
            throw std::runtime_error(error.what());
        }
    }

    bool PythonRuntime::IsInitialized() const { return Py_IsInitialized() != 0; }
    bool PythonRuntime::HasInstances() const { return !PyClassMap.empty() || PythonBackend::HasLiveInstances(); }

    void PythonRuntime::Invoke(const UUID& uuid, LegacyScriptCallback callback, Entity* entity, float seconds)
    {
        Guard([&] {
            // Copy the root: a callback can remove its own ECS component and map
            // entry. Keep the receiver alive until the invocation returns.
            const auto receiver = PyClassMap.at(uuid);
            const char* name = nullptr;
            switch (callback)
            {
            case LegacyScriptCallback::Start: name = "Start"; break;
            case LegacyScriptCallback::Update: name = "Update"; break;
            case LegacyScriptCallback::FixedUpdate: name = "FixedUpdate"; break;
            case LegacyScriptCallback::UpdatePaused: name = "UpdatePaused"; break;
            case LegacyScriptCallback::UI: name = "OnUI"; break;
            case LegacyScriptCallback::Stop: name = "Stop"; break;
            }
            if (!py::hasattr(receiver, name)) return;
            auto function = receiver.attr(name);
            if (callback == LegacyScriptCallback::Start)
            {
                if (!entity || !*entity) throw std::runtime_error("Python Start entity is no longer valid.");
                // The callback may retain this wrapper beyond the caller's stack
                // frame. Its weak scene lifetime belongs to an owned Entity copy.
                function(py::cast(*entity, py::return_value_policy::copy));
            }
            else if (callback == LegacyScriptCallback::Update ||
                callback == LegacyScriptCallback::FixedUpdate ||
                callback == LegacyScriptCallback::UpdatePaused)
                function(Timestep(seconds));
            else function();
        });
    }

    void PythonRuntime::DispatchUI(const UUID& uuid, const std::string& source, const RmlUiEventPayloadData& data)
    {
        // Invoke separately so a legacy handler's exception does not suppress
        // the extended handler. Recheck ownership after a self-deleting handler.
        try
        {
            Guard([&] {
                const auto receiver = PyClassMap.at(uuid);
                if (py::hasattr(receiver, "OnUIEvent"))
                    receiver.attr("OnUIEvent")(data.DocumentId, source, data.EventType);
            });
        }
        catch (const std::exception& error) { HE_CORE_ERROR("[RmlUi] OnUIEvent failed: {0}", error.what()); }
        if (!IsClassRegistered(uuid)) return;
        Guard([&] {
            const auto receiver = PyClassMap.at(uuid);
            if (!py::hasattr(receiver, "OnUIEventEx")) return;
            py::dict payload;
            for (const auto& [name, value] : BuildRmlUiEventPayloadFields(data))
                std::visit([&](const auto& fieldValue) { payload[py::str(name)] = fieldValue; }, value);
            receiver.attr("OnUIEventEx")(data.DocumentId, data.TargetElementId, data.EventType, payload);
        });
    }

    void PythonRuntime::Execute(const std::string& code) { Guard([&] { py::exec(code); }); }

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

    static std::wstring PythonWidePath(const std::filesystem::path& path)
    {
#if defined(__APPLE__)
        wchar_t* decoded = Py_DecodeLocale(path.c_str(), nullptr);
        if (!decoded) throw std::runtime_error("Cannot decode a Python filesystem path");
        std::wstring result(decoded);
        PyMem_RawFree(decoded);
        return result;
#else
        return path.wstring();
#endif
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

        // Resolve script roots separately from the platform Python runtime.
        const std::wstring projectRootPath = PythonWidePath(ConfigManager::GetInstance().GetRootFolder());
        const std::wstring engineContentRootPath =
            PythonWidePath(ConfigManager::GetInstance().GetEngineContentFolder());
        const std::wstring projectScriptsPath =
            PythonWidePath(ConfigManager::GetProjectFullPath("Scripts"));
        const std::wstring engineScriptsPath =
            PythonWidePath(ConfigManager::GetEngineContentFullPath("Scripts"));
        const std::wstring scriptsLibPath =
            PythonWidePath(ConfigManager::GetEngineContentFullPath("Scripts/Lib"));
#if defined(__APPLE__)
        auto pythonHome = PlatformPaths::GetApplicationContentDirectory() / "Python";
#if !defined(HE_PACKAGED_GAME)
        if (!std::filesystem::is_directory(pythonHome))
            pythonHome = std::filesystem::u8path(HE_MACOS_PYTHON_HOME);
#endif
        if (!std::filesystem::is_directory(pythonHome / "lib/python3.11/encodings"))
        {
            PyConfig_Clear(&config);
            throw std::runtime_error("Python 3.11 standard library is missing: " + pythonHome.string());
        }
        const auto wideHome = PythonWidePath(pythonHome);
        PyStatus status = PyConfig_SetString(&config, &config.home, wideHome.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the macOS Python home");
#else
        PyStatus status = PyConfig_SetString(&config, &config.prefix, engineScriptsPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the CPython prefix");
        status = PyConfig_SetString(&config, &config.exec_prefix, engineScriptsPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the CPython executable prefix");
#endif

#if defined(HE_PACKAGED_GAME)
        // Scene components use package-qualified imports such as
        // "Scripts.SurvivorGame", so the cooked Content root must be present in
        // addition to the script and standard-library directories.
        status = PyWideStringList_Append(&config.module_search_paths, projectRootPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the packaged content path");
        status = PyWideStringList_Append(&config.module_search_paths, projectScriptsPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the packaged Python script path");
        status = PyWideStringList_Append(&config.module_search_paths, engineContentRootPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the packaged engine content path");
        status = PyWideStringList_Append(&config.module_search_paths, scriptsLibPath.c_str());
        ThrowIfPythonConfigFailed(config, status, "Failed to configure the packaged Python standard library path");
#if defined(__APPLE__)
        for (const auto& directory : {pythonHome / "lib/python3.11", pythonHome / "lib/python3.11/lib-dynload"})
        {
            const auto wideDirectory = PythonWidePath(directory);
            status = PyWideStringList_Append(&config.module_search_paths, wideDirectory.c_str());
            ThrowIfPythonConfigFailed(config, status, "Failed to configure the bundled macOS Python library");
        }
#endif
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

    void PythonRuntime::Init()
    {
        EnsurePythonModuleLinked();
        if (IsInitialized()) { CheckThread(); return; }
        mThread = std::this_thread::get_id();
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

    void PythonRuntime::Shutdown()
    {
        if (Py_IsInitialized() == 0)
            return;
        CheckThread();
        if (HasInstances())
            throw std::logic_error("Python runtime shutdown requires all script instances to be released.");

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

    bool PythonRuntime::RegisterModule(const UUID& uuid, const std::string& scriptPath)
    {
        CheckThread();
        py::gil_scoped_acquire acquire;
		RegistrationErrorMap.erase(uuid);
        try
        {
            const std::string pyModuleName = ScriptModuleName(scriptPath);
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
        catch (const std::invalid_argument& error)
        {
            HE_CORE_ERROR(error.what());
            RegistrationErrorMap[uuid] = error.what();
            PyClassMap.erase(uuid);
            ClassNameMap.erase(uuid);
            ModuleMap.erase(uuid);
            return false;
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

    std::string PythonRuntime::GetLastRegistrationError(const UUID& uuid) const
    {
        const auto error = RegistrationErrorMap.find(uuid);
        return error == RegistrationErrorMap.end() ? std::string{} : error->second;
    }

    bool PythonRuntime::IsModuleRegistered(const UUID& uuid) const
    {
        return ModuleMap.find(uuid) != ModuleMap.end();
    }

    bool PythonRuntime::IsClassRegistered(const UUID& uuid) const
    {
        return PyClassMap.find(uuid) != PyClassMap.end();
    }

    std::string PythonRuntime::GetPyClassName(const UUID& uuid) const
    {
        HE_CORE_ASSERT(ClassNameMap.find(uuid) != ClassNameMap.end() && "Can't find this member in Library!");
        return ClassNameMap[uuid];
    }

    void PythonRuntime::ReloadModule(const UUID& uuid, bool recreateClass)
    {
        Guard([&] {
            auto module = ModuleMap.at(uuid);
            module.reload();
            if (recreateClass)
                PyClassMap[uuid] = module.attr(ClassNameMap.at(uuid).c_str())();
        });
    }

    void PythonRuntime::UnregisterModule(const UUID& uuid)
    {
        CheckThread();
        py::gil_scoped_acquire acquire;
        auto modIt = ModuleMap.find(uuid);
        if (modIt != ModuleMap.end())
            ModuleMap.erase(modIt);

        ClassNameMap.erase(uuid);
        RegistrationErrorMap.erase(uuid);

        auto classIt = PyClassMap.find(uuid);
        if (classIt != PyClassMap.end())
            PyClassMap.erase(classIt);
    }

}
