#include "ScriptInspector.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Platform/PlatformProcess.h"
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Utils/PlatformUtils.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/FileUtils/FileWatcher.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <limits>
#include <type_traits>
#include <atomic>
#include <mutex>
#include <thread>
#include <fstream>
#include <sstream>

namespace HEngine
{
    static std::map<std::string, ScriptFieldDrawer> sFieldDrawers;
    void RegisterScriptFieldDrawer(std::string stableType, ScriptFieldDrawer drawer)
    {
        if (stableType.empty() || !drawer || sFieldDrawers.contains(stableType)) throw std::invalid_argument("Invalid or duplicate script field drawer identity");
        sFieldDrawers.emplace(std::move(stableType), std::move(drawer));
    }
    void UnregisterScriptFieldDrawer(const std::string& stableType) { sFieldDrawers.erase(stableType); }
    namespace
    {
        bool MatchesResourceType(const std::string& path, const std::string& type)
        {
            if (type.empty() || path.empty()) return true;
            static const std::map<std::string, std::set<std::string>> extensions{
                {"Texture", {".png", ".jpg", ".jpeg", ".bmp", ".hdr", ".tga", ".dds", ".ktx", ".ktx2"}},
                {"Prefab", {".hprefab"}}, {"Scene", {".he"}}, {"Material", {".hmat"}},
                {"Mesh", {".hmesh"}}, {"Skeleton", {".hskel"}}, {"AnimationClip", {".hanim"}},
                {"AnimationController", {".hanimctrl"}}, {"UiDocument", {".hui"}},
                {"Audio", {".wav", ".ogg", ".mp3", ".flac"}}, {"EffectPackage", {".efkpkg"}}
            };
            auto extension = std::filesystem::path(path).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto supported = extensions.find(type);
            return supported != extensions.end() && supported->second.contains(extension);
        }
        struct Compilation
        {
            bool RefreshModuleInputs = true;
            std::set<std::filesystem::path> WatchedInputs;
            struct Diagnostic { std::string Severity, Code, Module, File, Message; int Line = 0, Column = 0; std::string Log; };
            std::atomic<bool> Running = false;
            std::mutex Mutex;
            std::string Backend;
            bool RestartPlay = false;
            bool Consumed = true;
            bool SourcesChanged = false;
            std::unique_ptr<FileWatcher> Watcher;
            PlatformProcess::CapturedResult Result;
            std::vector<Diagnostic> Diagnostics;
            std::vector<std::string> Modules;
            std::string Log;
            std::vector<std::string> Source;
            std::string SourcePath;
            int SourceLine = 0;
            bool ShowSource = false, ScrollToSource = false;
            std::jthread Worker;
            ~Compilation() { Worker.request_stop(); if (Worker.joinable()) Worker.join(); }
        };
        Compilation& GetCompilation() { static Compilation compilation; return compilation; }

        void DrawSource(Compilation& compilation)
        {
            if (!compilation.ShowSource) return;
            ImGui::SetNextWindowSize(ImGuiWrapper::S(800, 600), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Script diagnostic source", &compilation.ShowSource))
            {
                ImGui::TextWrapped("%s:%d", compilation.SourcePath.c_str(), compilation.SourceLine);
                ImGui::BeginChild("Source lines", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (std::size_t index = 0; index < compilation.Source.size(); ++index)
                {
                    const bool current = static_cast<int>(index + 1) == compilation.SourceLine;
                    if (current) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.6f, 0.2f, 1));
                    ImGui::Text("%5d  %s", static_cast<int>(index + 1), compilation.Source[index].c_str());
                    if (current && compilation.ScrollToSource) ImGui::SetScrollHereY(0.3f);
                    if (current) ImGui::PopStyleColor();
                }
                compilation.ScrollToSource = false;
                ImGui::EndChild();
            }
            ImGui::End();
        }

        void DrawCompiler(const IScriptBackend& backend)
        {
            auto& compilation = GetCompilation();
            DrawSource(compilation);
            const auto* compiler = dynamic_cast<const IScriptCompiler*>(&backend);
            if (!compiler) return;
            const auto command = compiler->CompileCommand();
            if (!command) return;
            if (compilation.SourcesChanged) ImGui::TextUnformatted("Script inputs changed; compile to update the loaded generation.");
            if (compilation.Running)
            {
                ImGui::TextUnformatted("Compiling scripts...");
                if (ImGui::Button("Cancel compilation")) compilation.Worker.request_stop();
            }
            else
            {
                bool requested = ImGui::Button("Compile Scripts");
                ImGuiWrapper::SameLine(0);
                const bool restart = ImGui::Button("Recompile and Restart Play");
                requested |= restart;
                if (requested)
                {
                compilation.RestartPlay = restart;
                compilation.SourcesChanged = false;
                compilation.Consumed = false;
                compilation.Diagnostics.clear();
                compilation.Modules.clear();
                compilation.Backend = backend.Descriptor().Id;
                compilation.Running = true;
                try
                {
                    compilation.Worker = std::jthread([command = *command, &compilation](std::stop_token stop) {
                        auto result = PlatformProcess::RunCaptured(command.Executable, command.Arguments, stop);
                        { std::lock_guard lock(compilation.Mutex); compilation.Result = std::move(result); }
                        compilation.Running = false;
                    });
                }
                catch (const std::exception& error)
                {
                    compilation.Running = false;
                    compilation.Result = {-1, false, error.what()};
                }
                }
            }
            ImGui::TextWrapped("Compile Scripts keeps the current Play code until Stop. Recompile and Restart Play restores the edit scene and starts a fresh session. Failed builds keep the last valid generation.");
            if (!compilation.Running && compilation.Backend == backend.Descriptor().Id)
            {
                std::lock_guard lock(compilation.Mutex);
                ImGui::TextUnformatted(compilation.Result.Cancelled ? "Compilation cancelled" :
                    compilation.Result.ExitCode == 0 ? "Compilation passed; generation queued" : "Compilation failed");
                if (ImGui::CollapsingHeader("Compiler diagnostics", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::TextWrapped("Full log: %s", compilation.Log.c_str());
                    for (const auto& module : compilation.Modules) ImGui::TextWrapped("%s", module.c_str());
                    for (std::size_t index = 0; index < compilation.Diagnostics.size(); ++index)
                    {
                        const auto& diagnostic = compilation.Diagnostics[index];
                        ImGui::PushID(static_cast<int>(index));
                        ImGui::TextWrapped("%s %s [%s]: %s", diagnostic.Severity.c_str(), diagnostic.Code.c_str(), diagnostic.Module.c_str(), diagnostic.Message.c_str());
                        if (!diagnostic.Log.empty() && diagnostic.Log != compilation.Log) ImGui::TextWrapped("Module log: %s", diagnostic.Log.c_str());
                        const auto location = diagnostic.File + ":" + std::to_string(diagnostic.Line) + ":" + std::to_string(diagnostic.Column);
                        if (ImGui::Selectable(location.c_str()))
                        {
                            compilation.Source.clear();
                            compilation.SourcePath = diagnostic.File;
                            compilation.SourceLine = diagnostic.Line;
                            const auto path = std::filesystem::u8path(diagnostic.File);
                            std::error_code error;
                            if (std::filesystem::file_size(path, error) <= 1024 * 1024 && !error)
                            {
                                std::ifstream stream(path);
                                for (std::string line; std::getline(stream, line);) compilation.Source.push_back(std::move(line));
                            }
                            if (compilation.Source.empty()) compilation.Source.push_back("Source unavailable or exceeds the 1 MiB viewer limit.");
                            compilation.ShowSource = compilation.ScrollToSource = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::BeginChild("Compiler output", ImVec2(0, ImGuiWrapper::S(220.0f)), true);
                    ImGui::TextUnformatted(compilation.Result.Output.c_str());
                    ImGui::EndChild();
                }
            }
        }

        bool HasExpectedType(const ScriptValue& value, const ScriptValue& expected)
        {
            if (value.index() != expected.index()) return false;
            if (const auto* enumeration = std::get_if<ScriptEnum>(&value))
                return enumeration->Type == std::get<ScriptEnum>(expected).Type;
            if (const auto* aggregate = std::get_if<ScriptAggregate>(&value))
                return aggregate->Kind == std::get<ScriptAggregate>(expected).Kind && aggregate->Type == std::get<ScriptAggregate>(expected).Type;
            return true;
        }

        bool EditString(const char* label, std::string& value, bool multiline = false)
        {
            value.reserve(std::max<std::size_t>(value.capacity(), 256));
            const auto resize = [](ImGuiInputTextCallbackData* data) {
                    auto& text = *static_cast<std::string*>(data->UserData);
                    text.resize(data->BufTextLen);
                    data->Buf = text.data();
                    return 0;
                };
            if (multiline) return ImGui::InputTextMultiline(label, value.data(), value.capacity() + 1,
                ImGuiWrapper::S(0, 90), ImGuiInputTextFlags_CallbackResize, resize, &value);
            return ImGui::InputText(label, value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize, resize, &value);
        }

        bool EditValue(ScriptValue& value, const ScriptField& field, Entity owner)
        {
            const auto type = std::get_if<ScriptAggregate>(&value);
            const auto enumeration = std::get_if<ScriptEnum>(&value);
            const std::string identity = type ? type->Type : enumeration ? enumeration->Type : std::string();
            if (const auto drawer = sFieldDrawers.find(identity); drawer != sFieldDrawers.end()) return drawer->second(value, field, owner);
            return std::visit([&](auto& item) -> bool {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, bool>) return ImGui::Checkbox("##value", &item);
                else if constexpr (std::is_same_v<T, std::int64_t>)
                {
                    const auto minimum = field.Min <= static_cast<double>(std::numeric_limits<std::int64_t>::min()) ?
                        std::numeric_limits<std::int64_t>::min() : field.Min >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) ?
                        std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(std::ceil(field.Min));
                    const auto maximum = field.Max >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) ?
                        std::numeric_limits<std::int64_t>::max() : field.Max <= static_cast<double>(std::numeric_limits<std::int64_t>::min()) ?
                        std::numeric_limits<std::int64_t>::min() : static_cast<std::int64_t>(std::floor(field.Max));
                    if (minimum > maximum) { ImGui::TextUnformatted("Range contains no integer value."); return false; }
                    const bool slider = field.HasRange && minimum >= std::numeric_limits<std::int64_t>::min() / 2 && maximum <= std::numeric_limits<std::int64_t>::max() / 2;
                    const bool changed = slider ? ImGui::SliderScalar("##value", ImGuiDataType_S64, &item, &minimum, &maximum) : ImGui::InputScalar("##value", ImGuiDataType_S64, &item);
                    if (changed) item = std::clamp(item, minimum, maximum);
                    return changed;
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    const bool changed = field.HasRange ? ImGui::SliderScalar("##value", ImGuiDataType_Double, &item, &field.Min, &field.Max) : ImGui::InputDouble("##value", &item);
                    if (changed) item = std::clamp(item, field.Min, field.Max);
                    return changed;
                }
                else if constexpr (std::is_same_v<T, std::string>) return EditString("##value", item, field.Multiline);
                else if constexpr (std::is_same_v<T, std::array<float, 2>>) return ImGui::InputFloat2("##value", item.data());
                else if constexpr (std::is_same_v<T, std::array<float, 3>>) return ImGui::InputFloat3("##value", item.data());
                else if constexpr (std::is_same_v<T, std::array<float, 4>>) return field.Color ? ImGui::ColorEdit4("##value", item.data()) : ImGui::InputFloat4("##value", item.data());
                else if constexpr (std::is_same_v<T, ScriptEnum>)
                {
                    const auto choice = std::find_if(field.EnumValues.begin(), field.EnumValues.end(), [&](const auto& option) { return option.second == item.Value; });
                    const auto label = choice == field.EnumValues.end() ? "Unknown value: " + std::to_string(item.Value) : choice->first;
                    bool changed = false;
                    if (ImGui::BeginCombo("##value", label.c_str()))
                    {
                        for (const auto& [name, number] : field.EnumValues)
                            if (ImGui::Selectable(name.c_str(), number == item.Value)) { item.Value = number; changed = true; }
                        ImGui::EndCombo();
                    }
                    return changed;
                }
                else if constexpr (std::is_same_v<T, ScriptAggregate>)
                {
                    auto document = nlohmann::json::parse(item.Json);
                    auto& children = document["value"][item.Kind == "struct" ? "fields" : "items"];
                    bool changed = false;
                    if (ImGui::TreeNode("##members", "%s (%zu)", item.Type.c_str(), children.size()))
                    {
                        for (std::size_t index = 0; index < children.size(); ++index)
                        {
                            auto it = children.begin(); std::advance(it, index);
                            const auto id = item.Kind == "struct" ? it.key() : std::to_string(index);
                            auto values = DecodeScriptFields(nlohmann::json{{"schema", 2}, {"fields", {{id, *it}}}}.dump());
                            ScriptField member; member.Id = id; member.Name = id; member.Default = values.at(id);
                            ImGui::PushID(id.c_str()); ImGui::TextUnformatted(id.c_str());
                            if (EditValue(values.at(id), member, owner)) { *it = nlohmann::json::parse(EncodeScriptFields(values))["fields"][id]; changed = true; }
                            if (item.Kind != "struct" && ImGui::SmallButton("Remove element"))
                            {
                                children.erase(it); changed = true; ImGui::PopID(); break;
                            }
                            ImGui::PopID();
                        }
                        if (item.Kind != "struct")
                        {
                            ImGui::BeginDisabled(children.size() >= 256 || !document["value"].contains("prototype"));
                            if (ImGui::SmallButton("Add element")) { children.push_back(document["value"]["prototype"]); changed = true; }
                            ImGui::EndDisabled();
                        }
                        ImGui::TreePop();
                    }
                    if (changed) item = ParseScriptAggregate(document.dump());
                    return changed;
                }
                else
                {
                    bool changed = EditString("##value", item.Id);
                    if (ImGui::BeginDragDropTarget())
                    {
                        if constexpr (std::is_same_v<T, ScriptEntityReference>)
                        {
                            if (const auto* payload = ImGui::AcceptDragDropPayload("SCENE_ENTITY_UUID"))
                                if (payload->DataSize > 1 && payload->DataSize < 128 && static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0')
                                {
                                    const std::string id(static_cast<const char*>(payload->Data));
                                    if (owner.GetLevel()->TryGetEntityByUUID(UUID(id))) { item.Id = id; changed = true; }
                                }
                        }
                        else if (const auto* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                        {
                            if (payload->DataSize >= sizeof(wchar_t) && payload->DataSize % sizeof(wchar_t) == 0 &&
                                static_cast<const wchar_t*>(payload->Data)[payload->DataSize / sizeof(wchar_t) - 1] == L'\0')
                            {
                                const auto relative = std::filesystem::path("Assets") / static_cast<const wchar_t*>(payload->Data);
                                const auto normalized = relative.lexically_normal();
                                if (!normalized.is_absolute() && *normalized.begin() == "Assets") { item.Id = normalized.generic_string(); changed = true; }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if constexpr (std::is_same_v<T, ScriptEntityReference>)
                    {
                        if (ImGui::BeginCombo("Pick entity", item.Id.c_str()))
                        {
                            for (auto raw : owner.GetLevel()->GetAllEntitiesWith<IDComponent>())
                            {
                                Entity candidate{raw, owner.GetLevel()};
                                const auto id = candidate.UUID().str();
                                const auto label = candidate.GetName() + "##" + id;
                                if (ImGui::Selectable(label.c_str(), item.Id == id)) { item.Id = id; changed = true; }
                            }
                            ImGui::EndCombo();
                        }
                        if (!item.Id.empty() && !owner.GetLevel()->TryGetEntityByUUID(UUID(item.Id))) ImGui::TextUnformatted("Missing entity; saved ID preserved.");
                    }
                    else
                    {
                        if (ImGui::Button("Pick resource"))
                        {
                            const auto selected = FileDialogs::OpenFile("Resource (*.*)\0*.*\0\0", ConfigManager::GetInstance().GetAssetsFolder().string());
                            if (!selected.empty())
                            {
                                const auto relative = selected.lexically_relative(ConfigManager::GetInstance().GetRootFolder());
                                if (!relative.empty() && *relative.begin() == "Assets") { item.Id = relative.generic_string(); changed = true; }
                            }
                        }
                        if (!item.Id.empty() && !std::filesystem::exists(ConfigManager::GetFullPath(item.Id))) ImGui::TextUnformatted("Missing resource; saved ID preserved.");
                    }
                    ImGuiWrapper::SameLine(0);
                    ImGui::BeginDisabled(!field.AllowNull);
                    if (ImGui::Button("Clear")) { item.Id.clear(); changed = true; }
                    ImGui::EndDisabled();
                    if (item.Id.empty() && !field.AllowNull) ImGui::TextUnformatted("This field requires a reference.");
                    return changed;
                }
            }, value);
        }
    }

    std::optional<ScriptCompileCompletion> PollScriptCompilation()
    {
        auto& compilation = GetCompilation();
        if (!compilation.Watcher)
        {
            compilation.Watcher = std::make_unique<FileWatcher>();
            const auto root = ConfigManager::GetInstance().GetRootFolder();
            compilation.Watcher->AddFile(std::filesystem::path(root) / "Scripts");
            compilation.Watcher->AddFile(std::filesystem::path(root) / "Config");
            const auto engine = ConfigManager::GetEngineFolder();
            compilation.Watcher->AddFile(engine / "Managed");
            compilation.Watcher->AddFile(engine.parent_path() / "Tools/Scripting");
            compilation.Watcher->GetFileModifiedEvent().Add([&compilation](const std::filesystem::path& path) {
                for (const auto& part : path)
                    if (part == "bin" || part == "obj") return;
                const auto extension = path.extension();
                if (extension == ".hemodule") compilation.RefreshModuleInputs = true;
                if (extension == ".cs" || extension == ".csproj" || extension == ".hemodule" || extension == ".props" || extension == ".targets" || extension == ".json" || extension == ".cmake" || extension == ".py" || extension == ".ps1")
                    compilation.SourcesChanged = true;
            });
        }
        compilation.Watcher->Update();
        if (compilation.RefreshModuleInputs)
        {
            compilation.RefreshModuleInputs = false;
            const auto root = ConfigManager::GetInstance().GetRootFolder();
            std::error_code error;
            for (const auto& file : std::filesystem::recursive_directory_iterator(root / "Scripts", error))
            {
                if (file.path().extension() != ".hemodule") continue;
                try
                {
                    if (file.file_size() > 65536) continue; // Compiler owns malformed module diagnostics.
                    std::ifstream input(file.path());
                    const auto module = nlohmann::json::parse(input);
                    for (const auto& item : module.value("generatedInputs", nlohmann::json::array()))
                    {
                        std::filesystem::path full; std::string normalized;
                        if (!Utils::TryResolveProjectRelativePath(root, item.get<std::string>(), full, normalized)) continue;
                        if (compilation.WatchedInputs.size() >= 1024 || !compilation.WatchedInputs.insert(full).second) continue;
                        compilation.Watcher->AddFile(full.parent_path(), false);
                        compilation.Watcher->AddCallBack(full, [&compilation](const std::filesystem::path&) { compilation.SourcesChanged = true; });
                    }
                }
                catch (const std::exception&) { compilation.SourcesChanged = true; }
            }
        }
        if (compilation.Running || compilation.Consumed) return {};
        std::lock_guard lock(compilation.Mutex);
        compilation.Consumed = true;
        const std::string diagnosticMarker = "HE_SCRIPT_DIAGNOSTICS=";
        if (const auto start = compilation.Result.Output.rfind(diagnosticMarker); start != std::string::npos)
        {
            const auto begin = start + diagnosticMarker.size();
            const auto end = compilation.Result.Output.find_first_of("\r\n", begin);
            const auto path = std::filesystem::u8path(compilation.Result.Output.substr(begin, end - begin));
            try
            {
                if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Compiler diagnostic receipt exceeds 1 MiB");
                std::ifstream stream(path);
                const auto receipt = nlohmann::json::parse(stream);
                compilation.Log = receipt.at("log").get<std::string>();
                for (const auto& module : receipt.value("modules", nlohmann::json::array()))
                    compilation.Modules.push_back(module.value("id", "") + " [" + module.value("assembly", "") + "]: " + module.value("status", "unknown"));
                for (const auto& item : receipt.at("diagnostics"))
                    compilation.Diagnostics.push_back({item.value("severity", "error"), item.value("code", ""), item.value("module", ""),
                        item.value("file", ""), item.value("message", ""), item.value("line", 0), item.value("column", 0), item.value("log", compilation.Log)});
            }
            catch (const std::exception& error) { compilation.Result.Output += std::string("\nDiagnostic receipt: ") + error.what(); }
        }
        if (compilation.Result.Cancelled || compilation.Result.ExitCode != 0) return {};
        const std::string marker = "HE_SCRIPT_OUTPUT=";
        const auto start = compilation.Result.Output.rfind(marker);
        if (start == std::string::npos) return {};
        const auto begin = start + marker.size();
        const auto end = compilation.Result.Output.find_first_of("\r\n", begin);
        return ScriptCompileCompletion{std::filesystem::u8path(compilation.Result.Output.substr(begin, end - begin)), compilation.RestartPlay};
    }

    void StopScriptCompilation()
    {
        auto& compilation = GetCompilation();
        compilation.Worker.request_stop();
        if (compilation.Worker.joinable()) compilation.Worker.join();
        compilation.Watcher.reset();
        compilation.Consumed = true;
    }

    bool DrawScriptInspector(ScriptComponent& component, Entity owner, std::span<const ScriptFields> selection, std::set<std::string>* editedFields, bool* resetPerformed)
    {
        struct Draft { std::string Original, Text; bool Reset = false; std::set<std::string> Touched; };
        static std::map<std::string, Draft> drafts;
        static std::map<std::string, std::string> errors;
        const auto ownerKey = owner.UUID().str();
        if (auto error = errors.find(ownerKey); error != errors.end()) ImGui::TextWrapped("Previous edit rejected: %s", error->second.c_str());
        bool resetRequested = false;
        ImGui::TextUnformatted(ModeManager::IsEditState() ? "Edit values (saved with scene)" : "Play values (discarded on Stop unless explicitly applied)");
        bool changed = ImGui::Checkbox("Enabled", &component.Enabled);
        if (ImGui::BeginCombo("Backend", component.Backend.c_str()))
        {
            for (const auto& descriptor : ScriptBackendRegistry::Describe())
            {
                if (ImGui::Selectable(descriptor.Id.c_str(), descriptor.Id == component.Backend) && descriptor.Id != component.Backend)
                {
                    component.Backend = descriptor.Id;
                    component.Type.clear();
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        const auto backend = ScriptBackendRegistry::Find(component.Backend);
        if (!backend)
        {
            ImGui::TextWrapped("Plugin unavailable: %s. Script type and fields are preserved.", component.Backend.c_str());
            ImGui::TextUnformatted(component.Type.c_str());
            return changed;
        }
        try
        {
            DrawCompiler(*backend);
            const auto types = backend->Types();
            if (ImGui::BeginCombo("Script Type", component.Type.c_str()))
            {
                for (const auto& type : types)
                    if (ImGui::Selectable(type.Id.c_str(), type.Id == component.Type) && type.Id != component.Type)
                    {
                        resetRequested = component.Type.empty();
                        component.Type = type.Id;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            const auto type = std::find_if(types.begin(), types.end(), [&](const ScriptType& candidate) { return candidate.Id == component.Type; });
            if (type == types.end())
            {
                ImGui::TextWrapped("Script type is missing or has not compiled: %s", component.Type.c_str());
                return changed;
            }
            if (!type->ModuleId.empty()) ImGui::TextWrapped("Loaded module: %s; execution rank: %d", type->ModuleId.c_str(), type->ExecutionOrder);
            const auto draftKey = owner.UUID().str() + ":" + component.Backend + ":" + component.Type + (ModeManager::IsEditState() ? ":edit" : ":play");
            if (auto draft = drafts.find(draftKey); draft != drafts.end() && draft->second.Original != component.Fields) drafts.erase(draft);
            auto fields = DecodeScriptFields(drafts.contains(draftKey) ? drafts.at(draftKey).Text : component.Fields);
            bool fieldsChanged = false;
            std::set<std::string> touched;
            if (ImGui::Button("Reset serialized fields") || resetRequested)
            {
                for (const auto& field : type->Fields) { fields.insert_or_assign(field.Id, field.Default); touched.insert(field.Id); }
                fieldsChanged = true;
                resetRequested = true;
            }
            for (const auto& field : type->Fields)
            {
                if (field.HideInInspector) continue;
                auto it = fields.find(field.Id);
                if (it == fields.end())
                {
                    ScriptValue value = field.Default;
                    for (const auto& oldId : field.FormerIds)
                        // Preserve a renamed value even when its type changed:
                        // the mismatch must remain visible until explicitly reset.
                        if (const auto old = fields.find(oldId); old != fields.end()) { value = old->second; break; }
                    it = fields.emplace(field.Id, std::move(value)).first;
                }
                ImGui::PushID(field.Id.c_str());
                if (!field.Header.empty()) ImGui::SeparatorText(field.Header.c_str());
                if (field.Space > 0) ImGui::Dummy(ImGuiWrapper::S(0, static_cast<float>(field.Space)));
                ImGui::TextUnformatted(field.Name.c_str());
                const auto displayed = EncodeScriptFields({{field.Id, it->second}});
                const bool mixed = std::any_of(selection.begin(), selection.end(), [&](const auto& other) {
                    auto value = other.find(field.Id);
                    return EncodeScriptFields({{field.Id, value == other.end() ? field.Default : value->second}}) != displayed;
                });
                if (mixed) { ImGui::SameLine(); ImGui::TextDisabled("(mixed values)"); }
                if (!field.Tooltip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", field.Tooltip.c_str());
                ImGui::BeginDisabled(field.ReadOnly);
                bool edited = false;
                if (!HasExpectedType(it->second, field.Default))
                {
                    ImGui::TextWrapped("Saved field type differs from the compiled script.");
                    if (ImGui::Button("Use Default")) { it->second = field.Default; edited = true; }
                }
                else
                {
                    edited = EditValue(it->second, field, owner);
                    if (ImGui::SmallButton("Use Default")) { it->second = field.Default; edited = true; }
                }
                if (edited) { fieldsChanged = true; touched.insert(field.Id); }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            if (fieldsChanged)
            {
                errors.erase(ownerKey);
                auto& draft = drafts[draftKey];
                draft.Original = component.Fields;
                draft.Text = EncodeScriptFields(fields);
                draft.Reset |= resetRequested;
                draft.Touched.insert(touched.begin(), touched.end());
            }
            if (!ImGui::IsAnyItemActive())
            {
                if (auto draft = drafts.find(draftKey); draft != drafts.end())
                {
                    // One atomic edit: validation runs after the input transaction,
                    // never for every character or slider sample. Keep the original
                    // serialized data if validation fails.
                    auto values = DecodeScriptFields(draft->second.Text);
                    const bool reset = draft->second.Reset;
                    const auto committedFields = draft->second.Touched;
                    drafts.erase(draft);
                    if (ModeManager::IsEditState())
                        if (auto* editor = dynamic_cast<IScriptEditorFields*>(backend.get())) values = editor->EditFields(component.Type, values, reset);
                    for (const auto& field : type->Fields)
                    {
                        if (!committedFields.contains(field.Id)) continue;
                        const auto value = values.find(field.Id);
                        if (value == values.end()) continue;
                        if (const auto* resource = std::get_if<ScriptResourceReference>(&value->second))
                        {
                            if (resource->Id.empty() && !field.AllowNull) throw std::invalid_argument(field.Name + ": a resource reference is required.");
                            if (!MatchesResourceType(resource->Id, field.ResourceType)) throw std::invalid_argument(field.Name + ": expected resource type " + field.ResourceType + "; original value retained.");
                        }
                        if (const auto* reference = std::get_if<ScriptEntityReference>(&value->second))
                            if (reference->Id.empty() && !field.AllowNull) throw std::invalid_argument(field.Name + ": an entity reference is required.");
                    }
                    component.Fields = EncodeScriptFields(values);
                    if (editedFields) editedFields->insert(committedFields.begin(), committedFields.end());
                    if (resetPerformed) *resetPerformed = reset;
                    changed = true;
                }
                if (drafts.size() > 128) drafts.clear();
            }
        }
        catch (const std::exception& error)
        {
            if (errors.size() >= 128) errors.clear();
            errors[ownerKey] = error.what();
            ImGui::TextWrapped("Script error: %s", error.what());
        }
        return changed;
    }
}
