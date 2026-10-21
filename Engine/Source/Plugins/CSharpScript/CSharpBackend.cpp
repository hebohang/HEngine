#include "CSharpBackend.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Profile/Profile.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Scripting/ScriptEngineApi.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"
#if defined(HE_SCRIPT_CSHARP_WASM)
#include "Runtime/Platform/Scripting/WasmCSharpHost.h"
#elif defined(__ANDROID__)
#include "Runtime/Platform/Android/AndroidManagedHost.h"
#else
#include "Runtime/Platform/PlatformPaths.h"
#include "CSharpBuildConfig.h"
#include <HEngine/ProjectBuildConfig.h>
#endif

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <fstream>

namespace HEngine
{
    namespace
    {
        ScriptAbi::Request Request(ScriptAbi::ManagedOperation operation, ScriptInstanceHandle instance = 0)
        {
            ScriptAbi::Request r;
            r.Version = ScriptAbi::Version;
            r.Operation = static_cast<std::uint32_t>(operation);
            r.Instance = instance;
            return r;
        }
    }

    std::string CSharpBackend::Call(ScriptAbi::Request& r, const std::string& name, const std::string& input) const
    {
        HE_PROFILE_SECTION("Script.CSharp.Dispatch");
        if (!mEntry) throw std::logic_error("C# runtime entry is not loaded.");
        if (name.size() > 4096 || input.size() > 1024 * 1024) throw std::invalid_argument("C# request exceeds its ABI size limit.");
        r.Name = reinterpret_cast<std::uintptr_t>(name.data()); r.NameLength = static_cast<std::int32_t>(name.size());
        r.Input = reinterpret_cast<std::uintptr_t>(input.data()); r.InputLength = static_cast<std::int32_t>(input.size());
        r.Output = reinterpret_cast<std::uintptr_t>(mOutput.data()); r.OutputCapacity = static_cast<std::int32_t>(mOutput.size());
        const auto status = mEntry(&r);
        if (r.OutputLength < 0 || r.OutputLength > r.OutputCapacity) throw std::runtime_error("C# runtime returned an invalid output length.");
        std::string text(mOutput.data(), r.OutputLength);
        if (status != 0)
        {
            const auto diagnostic = nlohmann::json::parse(text, nullptr, false);
            if (diagnostic.is_object() && diagnostic.value("scriptError", 0) == 1 &&
                diagnostic.contains("message") && diagnostic["message"].is_string() &&
                diagnostic.contains("stack") && diagnostic["stack"].is_string())
                throw ScriptException({ScriptErrorKind::RuntimeFailure,
                    diagnostic["message"].get<std::string>(), diagnostic["stack"].get<std::string>()});
            throw std::runtime_error(text.empty() ? "C# native call failed without diagnostic text." : text);
        }
        return text;
    }

    void CSharpBackend::EnsureLoaded() const
    {
        if (mInitialized) return;
        ScriptEngineApi::BindThread();
#if defined(HE_SCRIPT_CSHARP_WASM)
        mEntry = &CallWasmCSharp;
#elif defined(__ANDROID__)
        mEntry = GetAndroidManagedEntry();
#else
        const auto engineRoot = PlatformPaths::GetApplicationContentDirectory() / "Scripting" / "CSharp";
        auto root = engineRoot;
#if HE_PORTABLE_EDITOR || (HE_PORTABLE_PLAYER && !defined(HE_PACKAGED_GAME))
        const auto compiled = ConfigManager::GetSavedPath("Scripting/CSharp");
        const auto prebuilt = ConfigManager::GetProjectFullPath("Scripts/CSharp/Prebuilt");
        if (std::filesystem::is_regular_file(compiled / "current.json"))
        {
            std::ifstream stream(compiled / "current.json");
            const auto generation = nlohmann::json::parse(stream).at("generation").get<std::string>();
            if (generation.size() != 32 || generation.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::runtime_error("Invalid project C# generation pointer.");
            root = compiled / generation;
        }
        else if (std::filesystem::is_regular_file(compiled / "build.json")) root = compiled;
        else if (std::filesystem::is_regular_file(prebuilt / "build.json")) root = prebuilt;
#endif
#if WITH_EDITOR
        const auto assembly = root / "HEngine.ScriptHost.dll";
#else
        const auto assembly = root / "HEngine.ScriptRuntime.dll";
#endif
        const auto config = root / "HEngine.ScriptRuntime.runtimeconfig.json";
        auto runtimeRoot = engineRoot / "Runtime";
        #if defined(__APPLE__)
        auto hostFxr = runtimeRoot / "host" / "fxr" / "10.0.10" / "libhostfxr.dylib";
#else
        auto hostFxr = runtimeRoot / "host" / "fxr" / "10.0.10" / "hostfxr.dll";
#endif
#if !defined(HE_PACKAGED_GAME)
        if (!std::filesystem::exists(hostFxr)) { hostFxr = std::filesystem::u8path(HE_CSHARP_DEVELOPMENT_HOSTFXR); runtimeRoot.clear(); }
#endif
        auto host = std::make_unique<CoreClrHost>();
        host->Open(hostFxr, config, runtimeRoot);
#if WITH_EDITOR
        mEntry = reinterpret_cast<Entry>(host->LoadEntry(assembly, "HEngine.EditorScripting.NativeEntry, HEngine.ScriptHost", "Dispatch"));
#else
        mEntry = reinterpret_cast<Entry>(host->LoadEntry(assembly, "HEngine.Scripting.NativeEntry, HEngine.ScriptRuntime", "Dispatch"));
#endif
#endif
        auto describe = Request(ScriptAbi::ManagedOperation::Describe);
        const auto metadata = nlohmann::json::parse(Call(describe));
        if (metadata.at("version") != 1) throw std::runtime_error("Unsupported generated C# metadata version.");
        std::vector<ScriptType> types;
        for (const auto& entry : metadata.at("types"))
        {
            ScriptType type;
            type.Id = entry.at("id").get<std::string>();
            type.ExecutionOrder = entry.value("executionOrder", 0);
            type.ModuleId = entry.value("moduleId", "GameScripts");
            type.Resource = entry.at("resource").get<std::string>();
            type.Dependencies = entry.at("dependencies").get<std::vector<std::string>>();
            for (const auto& f : entry.at("fields"))
            {
                ScriptField field;
                field.Id = f.at("id").get<std::string>(); field.Name = f.at("name").get<std::string>();
                field.FormerIds = f.at("formerIds").get<std::vector<std::string>>();
                if (f.contains("ui"))
                {
                    const auto& ui = f.at("ui");
                    field.Tooltip = ui.value("tooltip", ""); field.Header = ui.value("header", "");
                    field.ResourceType = ui.value("resourceType", ""); field.AllowNull = ui.value("allowNull", true);
                    field.Space = ui.value("space", 0.0); field.Min = ui.value("min", field.Min); field.Max = ui.value("max", field.Max);
                    field.HasRange = ui.value("hasRange", false); field.ReadOnly = ui.value("readOnly", false);
                    field.HideInInspector = ui.value("hidden", false); field.Multiline = ui.value("multiline", false); field.Color = ui.value("color", false);
                }
                if (f.contains("enumValues"))
                    for (const auto& value : f.at("enumValues")) field.EnumValues.emplace_back(value.at("name").get<std::string>(), value.at("value").get<std::int64_t>());
                const nlohmann::json encoded{{"schema", 2}, {"fields", {{field.Id, f.at("defaultValue")}}}};
                field.Default = DecodeScriptFields(encoded.dump()).at(field.Id);
                type.Fields.push_back(std::move(field));
            }
            types.push_back(std::move(type));
        }
        auto initialize = Request(ScriptAbi::ManagedOperation::Initialize);
        initialize.HostEntry = reinterpret_cast<std::uintptr_t>(&ScriptEngineApi::Call);
        Call(initialize);
#if !defined(HE_SCRIPT_CSHARP_WASM) && !defined(__ANDROID__)
        mHost = std::move(host);
#endif
        mTypes = std::move(types);
        mInitialized = true;
        HE_CORE_INFO("[C#] .NET 10.0.10 loaded; types={0}; GameScripts SHA256={1}", mTypes.size(), metadata.at("identity").get<std::string>());
    }

#if WITH_EDITOR
    ScriptFields CSharpBackend::EditFields(const std::string& type, const ScriptFields& fields, bool reset)
    {
        if (!mInstances.empty()) throw std::logic_error("Edit validation requires a stopped scene.");
        EnsureLoaded();
        auto request = Request(ScriptAbi::ManagedOperation::EditFields);
        request.Callback = static_cast<std::uint32_t>(reset ? ScriptCallback::Reset : ScriptCallback::OnValidate);
        auto result = fields;
        for (auto& [id, value] : DecodeScriptFields(Call(request, type, EncodeScriptFields(fields))))
        {
            if (const auto old = fields.find(id); old != fields.end()) value = PreserveUnknownScriptMembers(old->second, std::move(value));
            result.insert_or_assign(id, std::move(value));
        }
        return result;
    }

    void CSharpBackend::Reload(const std::filesystem::path& directory)
    {
        if (!mInstances.empty()) throw std::logic_error("Stop all script scenes before reloading assemblies.");
        EnsureLoaded();
        const auto utf8 = directory.u8string();
        auto request = Request(ScriptAbi::ManagedOperation::Reload);
        Call(request, {}, std::string(utf8.begin(), utf8.end()));
        mInitialized = false;
        mTypes.clear();
        EnsureLoaded();
    }

    std::optional<ScriptCompileCommand> CSharpBackend::CompileCommand() const
    {
        const auto project = ConfigManager::GetInstance().GetRootFolder().u8string();
#if HE_PORTABLE_EDITOR
        const auto script = (ConfigManager::GetEngineFolder().parent_path() / "Tools/Scripting/CompileEditor.ps1").u8string();
        return ScriptCompileCommand{PlatformPaths::GetPowerShellExecutable(),
            {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", std::string(script.begin(), script.end()),
             "-Project", std::string(project.begin(), project.end()), "-Dotnet", "dotnet", "-CMake", "cmake"}};
#else
        return ScriptCompileCommand{std::filesystem::u8path(HE_CSHARP_EDITOR_POWERSHELL),
            {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", HE_CSHARP_EDITOR_SCRIPT,
             "-Project", std::string(project.begin(), project.end()), "-Dotnet", HE_CSHARP_EDITOR_DOTNET,
             "-CMake", HE_CSHARP_EDITOR_CMAKE}};
#endif
    }
#endif
    std::span<const ScriptType> CSharpBackend::Types() const { EnsureLoaded(); return mTypes; }
    ScriptInstanceHandle CSharpBackend::Create(const std::string& type, ScriptEntityHandle entity)
    {
        EnsureLoaded();
        auto r = Request(ScriptAbi::ManagedOperation::Create); r.Entity = entity;
        Call(r, type);
        if (!r.Result || !mInstances.insert(r.Result).second) throw std::runtime_error("C# returned a duplicate or invalid instance handle.");
        return r.Result;
    }
    void CSharpBackend::Write(ScriptInstanceHandle id, const std::string& field, const ScriptValue& value)
    {
        auto r = Request(ScriptAbi::ManagedOperation::WriteField, id);
        const auto fields = nlohmann::json::parse(EncodeScriptFields({{field, value}}));
        Call(r, field, fields.at("fields").at(field).dump());
    }
    ScriptValue CSharpBackend::Read(ScriptInstanceHandle id, const std::string& field)
    {
        auto r = Request(ScriptAbi::ManagedOperation::ReadField, id);
        const auto value = nlohmann::json::parse(Call(r, field));
        const nlohmann::json fields{{"schema", 2}, {"fields", {{field, value}}}};
        return DecodeScriptFields(fields.dump()).at(field);
    }
    ScriptFields CSharpBackend::ReadFields(ScriptInstanceHandle id)
    {
        auto r = Request(ScriptAbi::ManagedOperation::ReadFields, id);
        return DecodeScriptFields(Call(r));
    }
    void CSharpBackend::Invoke(ScriptInstanceHandle id, ScriptCallback callback, double seconds)
    {
        HE_PROFILE_TRANSIENT_SECTION(callback == ScriptCallback::Update ? "Script.CSharp.Update" : "Script.CSharp.Lifecycle");
        auto r = Request(ScriptAbi::ManagedOperation::Invoke, id);
        r.Callback = static_cast<std::uint32_t>(callback); r.Seconds = seconds;
        Call(r);
    }
    void CSharpBackend::Contact(ScriptInstanceHandle id, const ScriptContactEvent& contact)
    {
        auto r = Request(ScriptAbi::ManagedOperation::PhysicsContact, id);
        r.Entity = contact.Other; r.Callback = contact.Phase; r.Flags = contact.Trigger ? 1u : 0u;
        for (size_t i = 0; i < 3; ++i) { r.Data[i] = contact.Point[i]; r.Data[i + 3] = contact.Normal[i]; }
        Call(r);
    }
    void CSharpBackend::BeginFrame(double seconds, bool paused)
    {
        auto request = Request(ScriptAbi::ManagedOperation::BeginFrame);
        request.Seconds = seconds; request.Flags = paused ? 1u : 0u;
        request.Callback = request.Flags;
        Call(request);
    }
    void CSharpBackend::Release(ScriptInstanceHandle id) noexcept
    {
        if (!mInstances.contains(id)) return;
        try { auto r = Request(ScriptAbi::ManagedOperation::Release, id); Call(r); mInstances.erase(id); }
        catch (const std::exception& error) { Log::WriteFallbackError("C# root release failed.", error.what()); }
    }
    void CSharpBackend::Shutdown()
    {
        if (!mInstances.empty()) throw std::logic_error("Cannot stop C# with live script roots.");
        if (mInitialized) { auto r = Request(ScriptAbi::ManagedOperation::Shutdown); Call(r); }
        mInitialized = false; mEntry = nullptr; mTypes.clear();
#if !defined(HE_SCRIPT_CSHARP_WASM) && !defined(__ANDROID__)
        mHost.reset();
#endif
    }

    std::uint64_t CSharpBackend::Collect()
    {
        EnsureLoaded();
        auto r = Request(ScriptAbi::ManagedOperation::Collect);
        Call(r);
        return r.Result;
    }
}
