#include "PythonBackend.h"
#include "Binding/ScriptApiBinding.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Scripting/ScriptEngine.h"
#include "Runtime/Scripting/ScriptEngineApi.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"
#include "Runtime/Scripting/ScriptWorld.h"

#include <nlohmann/json.hpp>
#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <thread>

namespace HEngine
{
    namespace py = pybind11;
    namespace
    {
        std::size_t sLiveInstances = 0;
        ScriptException Diagnostic(py::error_already_set& error)
        {
            // Caller owns the GIL. The engine ships a minimal standard library
            // without traceback.py. Reuse pybind11's native frame formatter,
            // which already captures real filenames, line numbers and functions.
            const std::string fallback = error.what();
            try
            {
                return ScriptException({ScriptErrorKind::RuntimeFailure,
                    py::str(error.type().attr("__name__")).cast<std::string>() + ": " + py::str(error.value()).cast<std::string>(),
                    fallback});
            }
            catch (const std::exception&)
            {
                return ScriptException({ScriptErrorKind::RuntimeFailure, "Python exception (message formatting failed)", fallback});
            }
        }
        bool Identifier(const std::string& text)
        {
            const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
            return !text.empty() && text.size() <= 128 && letter(text.front()) && !text.starts_with("__") &&
                std::all_of(text.begin(), text.end(), [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
        }
        py::object ToPython(const ScriptValue& value)
        {
            return std::visit([](const auto& item) -> py::object {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, ScriptEntityReference> || std::is_same_v<T, ScriptResourceReference>) return py::str(item.Id);
                else if constexpr (std::is_same_v<T, ScriptEnum>) return py::int_(item.Value);
                else if constexpr (std::is_same_v<T, ScriptAggregate>) throw std::invalid_argument("Python aggregate fields require an explicit Python schema binding.");
                else return py::cast(item);
            }, value);
        }
        ScriptValue FromPython(const py::object& value, const ScriptValue& prototype)
        {
            return std::visit([&](const auto& item) -> ScriptValue {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, bool>)
                {
                    if (!py::isinstance<py::bool_>(value)) throw std::invalid_argument("Python field requires bool.");
                    return value.cast<bool>();
                }
                else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, ScriptEnum>)
                {
                    if (!py::isinstance<py::int_>(value) || py::isinstance<py::bool_>(value)) throw std::invalid_argument("Python field requires a signed 64-bit integer.");
                    if constexpr (std::is_same_v<T, ScriptEnum>) return ScriptEnum{item.Type, value.cast<std::int64_t>()};
                    else return value.cast<std::int64_t>();
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    if ((!py::isinstance<py::float_>(value) && !py::isinstance<py::int_>(value)) || py::isinstance<py::bool_>(value)) throw std::invalid_argument("Python field requires float64.");
                    const double number = value.cast<double>();
                    if (!std::isfinite(number)) throw std::invalid_argument("Python field is non-finite.");
                    return number;
                }
                else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, ScriptEntityReference> || std::is_same_v<T, ScriptResourceReference>)
                {
                    if (!py::isinstance<py::str>(value)) throw std::invalid_argument("Python field requires a UTF-8 string or stable reference ID.");
                    auto text = value.cast<std::string>();
                    if (text.size() > 1024 * 1024) throw std::invalid_argument("Python field exceeds 1 MiB.");
                    if constexpr (std::is_same_v<T, std::string>) return text;
                    else return T{std::move(text)};
                }
                else
                {
                    if constexpr (std::is_same_v<T, ScriptAggregate>) throw std::invalid_argument("Python aggregate fields require an explicit Python schema binding.");
                    else {
                    auto vector = value.cast<T>();
                    if (!std::all_of(vector.begin(), vector.end(), [](float number) { return std::isfinite(number); })) throw std::invalid_argument("Python vector is non-finite.");
                    return vector;
                    }
                }
            }, prototype);
        }
    }

    struct PythonBackend::Impl
    {
        struct Instance { py::object Object; std::size_t Type; ScriptEntityHandle Entity; };
        ScriptBackendDescriptor Descriptor{"he.script.python", 1, {ScriptPlatform::Windows}, {"CPython 3.11"}, false, false, ScriptRuntimeLoading::LinkedInterpreter};
        std::thread::id Thread = std::this_thread::get_id();
        std::vector<ScriptType> Types;
        std::vector<std::pair<std::string, std::string>> Sources;
        std::map<ScriptInstanceHandle, Instance> Instances;
        ScriptInstanceHandle Next = 1;
        bool Loaded = false;

        void Check() const
        {
            if (Thread != std::this_thread::get_id()) throw std::logic_error("Python backend requires its engine owning thread.");
        }
        void Load()
        {
            Check();
            if (Loaded) return;
            const auto path = ConfigManager::GetProjectFullPath("Scripts/PythonScripts.json");
            if (!std::filesystem::exists(path)) { Loaded = true; return; }
            if (std::filesystem::file_size(path) > 1024 * 1024) throw std::invalid_argument("Python catalog exceeds 1 MiB.");
            std::ifstream stream(path);
            const auto catalog = nlohmann::json::parse(stream);
            if (catalog.at("version") != 1) throw std::invalid_argument("Unsupported Python script catalog version.");
            std::vector<ScriptType> types;
            std::vector<std::pair<std::string, std::string>> sources;
            for (const auto& entry : catalog.at("types"))
            {
                ScriptType type;
                type.Id = entry.at("id").get<std::string>();
                type.Resource = entry.at("resource").get<std::string>();
                if (type.Id.empty() || type.Id.size() > 128 || std::any_of(types.begin(), types.end(), [&](const auto& previous) { return previous.Id == type.Id; })) throw std::invalid_argument("Empty or duplicate Python script type ID.");
                auto resource = std::filesystem::path(type.Resource);
                if (!type.Resource.starts_with("Scripts/") || resource.is_absolute() || resource.extension() != ".py" || type.Resource.find('\\') != std::string::npos) throw std::invalid_argument("Python script resource must be Scripts/<module>.py.");
                std::string module;
                for (const auto& part : resource.replace_extension())
                {
                    const auto name = part.string();
                    if (!Identifier(name)) throw std::invalid_argument("Python script resource has an invalid module segment.");
                    if (!module.empty()) module += '.';
                    module += name;
                }
                const auto className = entry.at("class").get<std::string>();
                if (!Identifier(className)) throw std::invalid_argument("Invalid Python script class name.");
                type.Dependencies = entry.value("dependencies", std::vector<std::string>{});
                for (const auto& f : entry.at("fields"))
                {
                    ScriptField field;
                    field.Id = f.at("id").get<std::string>(); field.Name = f.at("name").get<std::string>();
                    if (field.Id.empty() || !Identifier(field.Name) || field.Name == "Entity" || std::any_of(type.Fields.begin(), type.Fields.end(), [&](const auto& previous) { return previous.Id == field.Id || previous.Name == field.Name; })) throw std::invalid_argument("Invalid or duplicate Python field ID/name.");
                    field.FormerIds = f.value("formerIds", std::vector<std::string>{});
                    const nlohmann::json encoded{{"schema", 1}, {"fields", {{field.Id, f.at("defaultValue")}}}};
                    field.Default = DecodeScriptFields(encoded.dump()).at(field.Id);
                    type.Fields.push_back(std::move(field));
                }
                types.push_back(std::move(type)); sources.emplace_back(std::move(module), className);
            }
            Types = std::move(types); Sources = std::move(sources); Loaded = true;
        }
        const ScriptField& Field(ScriptInstanceHandle handle, const std::string& id)
        {
            const auto& fields = Types.at(Instances.at(handle).Type).Fields;
            const auto it = std::find_if(fields.begin(), fields.end(), [&](const auto& field) { return field.Id == id; });
            if (it == fields.end()) throw std::invalid_argument("Python field is not declared in its catalog: " + id);
            return *it;
        }
    };

    PythonBackend::PythonBackend() : mImpl(std::make_unique<Impl>()) {}
    PythonBackend::~PythonBackend() = default;
    bool PythonBackend::HasLiveInstances() { return sLiveInstances != 0; }
    const ScriptBackendDescriptor& PythonBackend::Descriptor() const { return mImpl->Descriptor; }
    std::span<const ScriptType> PythonBackend::Types() const { mImpl->Load(); return mImpl->Types; }
    ScriptInstanceHandle PythonBackend::Create(const std::string& type, ScriptEntityHandle entity)
    {
        mImpl->Load();
        if (!ScriptWorld::Resolve(entity)) throw std::invalid_argument("Python entity handle is stale.");
        const auto found = std::find_if(mImpl->Types.begin(), mImpl->Types.end(), [&](const auto& item) { return item.Id == type; });
        if (found == mImpl->Types.end()) throw std::invalid_argument("Unknown Python script type: " + type);
        if (mImpl->Instances.size() >= 100000 || mImpl->Next == std::numeric_limits<ScriptInstanceHandle>::max()) throw std::length_error("Python instance limit exceeded.");
        ScriptEngine::GetInstance().Init(); ScriptEngineApi::BindThread();
        py::gil_scoped_acquire acquire;
        try
        {
            py::module_::import("HEngine");
            const auto index = static_cast<std::size_t>(found - mImpl->Types.begin());
            const auto& [module, className] = mImpl->Sources.at(index);
            auto object = py::module_::import(module.c_str()).attr(className.c_str())();
            object.attr("Entity") = py::cast(PythonScriptEntity{entity});
            const auto handle = mImpl->Next++;
            mImpl->Instances.emplace(handle, Impl::Instance{std::move(object), index, entity});
            ++sLiveInstances;
            return handle;
        }
        catch (py::error_already_set& error) { throw Diagnostic(error); }
    }
    void PythonBackend::Write(ScriptInstanceHandle handle, const std::string& id, const ScriptValue& value)
    {
        mImpl->Check(); py::gil_scoped_acquire acquire;
        try
        {
            const auto& field = mImpl->Field(handle, id);
            if (field.Default.index() != value.index() || (std::holds_alternative<ScriptEnum>(value) &&
                std::get<ScriptEnum>(value).Type != std::get<ScriptEnum>(field.Default).Type))
                throw std::invalid_argument("Python field value differs from the declared type: " + id);
            mImpl->Instances.at(handle).Object.attr(field.Name.c_str()) = ToPython(value);
        }
        catch (py::error_already_set& error) { throw Diagnostic(error); }
    }
    ScriptValue PythonBackend::Read(ScriptInstanceHandle handle, const std::string& id)
    {
        mImpl->Check(); py::gil_scoped_acquire acquire;
        try { const auto& field = mImpl->Field(handle, id); return FromPython(mImpl->Instances.at(handle).Object.attr(field.Name.c_str()), field.Default); }
        catch (py::error_already_set& error) { throw Diagnostic(error); }
    }
    void PythonBackend::Invoke(ScriptInstanceHandle handle, ScriptCallback callback, double seconds)
    {
        mImpl->Check(); py::gil_scoped_acquire acquire;
        const PythonScriptContext context(mImpl->Instances.at(handle).Entity);
        try
        {
            const auto receiver = mImpl->Instances.at(handle).Object;
            const char* names[] = {"Initialize", "Enable", "Update", "Disable", "Destroy",
                "Start", "FixedUpdate", "LateUpdate", "Reset", "OnValidate", "Pause", "Resume",
                "ApplicationQuit", "SceneLoaded", "SceneUnloading"};
            const auto index = static_cast<std::size_t>(callback);
            if (index >= std::size(names)) throw std::invalid_argument("Invalid Python lifecycle callback.");
            if (!py::hasattr(receiver, names[index])) return;
            if (callback == ScriptCallback::Update || callback == ScriptCallback::FixedUpdate || callback == ScriptCallback::LateUpdate)
                receiver.attr(names[index])(seconds);
            else receiver.attr(names[index])();
        }
        catch (py::error_already_set& error) { throw Diagnostic(error); }
    }
    void PythonBackend::Contact(ScriptInstanceHandle handle, const ScriptContactEvent& contact)
    {
        mImpl->Check(); py::gil_scoped_acquire acquire;
        const PythonScriptContext context(mImpl->Instances.at(handle).Entity);
        try
        {
            const auto receiver = mImpl->Instances.at(handle).Object;
            const char* method = contact.Trigger ? "Trigger" : "Collision";
            if (py::hasattr(receiver, method)) receiver.attr(method)(py::cast(contact));
        }
        catch (py::error_already_set& error) { throw Diagnostic(error); }
    }
    void PythonBackend::Release(ScriptInstanceHandle handle) noexcept
    {
        try
        {
            mImpl->Check(); py::gil_scoped_acquire acquire;
            if (mImpl->Instances.erase(handle)) --sLiveInstances;
        }
        catch (const std::exception& error) { HE_CORE_ERROR("Python instance release failed: {0}", error.what()); }
    }
}
