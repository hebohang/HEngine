#include "Hepch.h"

#include "Runtime/Graphics/Settings/PipelineProfile.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <stdexcept>
#include <set>

namespace HEngine
{
    namespace
    {
        void ValidateName(std::string_view name)
        {
            if (name.empty() || name.size() > 96 || name == "." || name == ".." ||
                name.find_first_of("/\\:*?\"<>|") != std::string_view::npos ||
                name.back() == '.' || name.back() == ' ' ||
                std::ranges::any_of(name, [](unsigned char c) { return c < 32; }))
                throw std::runtime_error("Profile name must be a valid file name of 1 to 96 characters.");
        }

        bool IsProfileSetting(const RenderSettingDescriptor& descriptor)
        {
            return descriptor.bPersistedRenderSetting &&
                std::ranges::find(descriptor.WritableLayers, SettingsLayer::Camera) != descriptor.WritableLayers.end() &&
                descriptor.PropertyId != RenderSettingIds::PipelineProfile &&
                !descriptor.PropertyId.starts_with("Rendering.Presentation.");
        }
    }

    Json SerializePipelineProfile(const PipelineProfile& profile)
    {
        ValidateName(profile.Name);
        return { { "Schema", "hengine.pipeline-profile" }, { "SchemaVersion", 1 },
            { "Name", profile.Name }, { "Settings", profile.Values } };
    }

    PipelineProfile ParsePipelineProfile(const Json& document)
    {
        if (!document.is_object() || document.value("Schema", "") != "hengine.pipeline-profile")
            throw std::runtime_error("Not an HEngine Pipeline Profile.");
        const int version = document.value("SchemaVersion", 0);
        if (version < 0 || version > 1)
            throw std::runtime_error("Unsupported Pipeline Profile schema version; original asset preserved.");
        PipelineProfile result;
        result.Name = document.value("Name", "Default");
        ValidateName(result.Name);
        // Version zero used Values; migration never changes the source document.
        result.Values = document.value(version == 0 ? "Values" : "Settings", Json::object());
        if (!result.Values.is_object()) throw std::runtime_error("Profile Settings must be an object.");
        if (document.contains("AliasTarget"))
        {
            if (!document["AliasTarget"].is_string()) throw std::runtime_error("Profile alias target must be a name.");
            ValidateName(document["AliasTarget"].get<std::string>());
        }
        (void)BuildPipelineProfilePatch(result);
        return result;
    }

    PipelineProfile CapturePipelineProfile(std::string name, const RenderSettings& settings)
    {
        ValidateName(name);
        PipelineProfile result{ std::move(name) };
        const Json values = RenderSettingsToFlatValues(settings);
        for (const auto& descriptor : GetRenderSettingDescriptors())
            if (IsProfileSetting(descriptor)) result.Values[descriptor.PropertyId] = values.at(descriptor.PropertyId);
        return result;
    }

    Json BuildPipelineProfilePatch(const PipelineProfile& profile)
    {
        Json patch = Json::object();
        for (const auto& [id, value] : profile.Values.items())
        {
            const auto* descriptor = FindRenderSettingDescriptor(id);
            if (!descriptor) continue; // Retained in the asset, not sent to an older settings service.
            if (!IsProfileSetting(*descriptor)) throw std::runtime_error("Profile cannot override device or tool setting: " + id);
            patch[id] = value;
        }
        std::vector<RenderSettingDiagnostic> diagnostics;
        (void)ApplyRenderSettingsFlatValues(MakeEmergencyFallbackRenderSettings(), patch, diagnostics);
        if (HasDiagnosticErrors(diagnostics)) throw std::runtime_error("Invalid Pipeline Profile settings.");
        patch[RenderSettingIds::PipelineProfile] = profile.Name;
        return patch;
    }

    PipelineProfile MakeBuiltinPipelineProfile(std::string_view name)
    {
        auto settings = MakeEmergencyFallbackRenderSettings();
        if (name == "Low Cost")
        {
            settings.Path = RenderingPath::Forward;
            settings.AmbientOcclusion.bEnabled = false;
            settings.Reflections.bScreenSpaceEnabled = false;
            settings.Reflections.bLocalProbesEnabled = false;
            settings.GlobalIllumination.Technique = GlobalIlluminationTechniqueKind::Off;
            settings.AntiAliasing.Mode = AntiAliasingMode::Fxaa;
        }
        else if (name == "High Quality")
        {
            settings.Path = RenderingPath::Deferred;
            settings.AntiAliasing.Mode = AntiAliasingMode::Taa;
            settings.AmbientOcclusion.bEnabled = true;
            settings.GlobalIllumination.Technique = GlobalIlluminationTechniqueKind::ScreenSpace;
        }
        else if (name != "Default") throw std::runtime_error("Unknown built-in Pipeline Profile.");
        return CapturePipelineProfile(std::string(name), settings);
    }

    std::filesystem::path PipelineProfileDirectory()
    {
        return ConfigManager::GetProjectFullPath("Assets/Rendering/PipelineProfiles");
    }

    std::vector<std::string> ListPipelineProfiles()
    {
        std::vector<std::string> names;
        const auto directory = PipelineProfileDirectory();
        if (!std::filesystem::exists(directory)) return names;
        for (const auto& item : std::filesystem::directory_iterator(directory))
            if (item.is_regular_file() && item.path().extension() == ".hepipeline")
            {
                const Json document = Utils::LoadJsonFile(item.path());
                (void)ParsePipelineProfile(document);
                if (document.contains("AliasTarget"))
                {
                    (void)LoadPipelineProfileAsset(item.path());
                    continue;
                }
                names.push_back(item.path().stem().string());
            }
        std::ranges::sort(names);
        return names;
    }

    PipelineProfile LoadPipelineProfile(std::string_view name)
    {
        ValidateName(name);
        return LoadPipelineProfileAsset(PipelineProfileDirectory() / (std::string(name) + ".hepipeline"));
    }

    PipelineProfile LoadPipelineProfileAsset(const std::filesystem::path& path)
    {
        auto current = path;
        std::set<std::string> visited;
        for (size_t depth = 0; depth < 64; ++depth)
        {
            if (!visited.insert(Utils::PathToUtf8(current.lexically_normal())).second)
                throw std::runtime_error("Pipeline Profile alias cycle.");
            const Json document = Utils::LoadJsonFile(current);
            auto profile = ParsePipelineProfile(document);
            if (!document.contains("AliasTarget")) return profile;
            current = current.parent_path() / (document["AliasTarget"].get<std::string>() + ".hepipeline");
        }
        throw std::runtime_error("Pipeline Profile alias chain exceeds 64 entries.");
    }

    void RenamePipelineProfile(std::string_view oldName, std::string_view newName)
    {
        ValidateName(oldName);
        ValidateName(newName);
        if (oldName == newName) return;
        auto profile = LoadPipelineProfile(oldName);
        const auto oldPath = PipelineProfileDirectory() / (std::string(oldName) + ".hepipeline");
        Json alias = Utils::LoadJsonFile(oldPath);
        profile.Name = newName;
        SavePipelineProfile(profile);
        // Publish the compatibility alias only after the target exists. Atomic write
        // failure keeps the original asset intact; no live reference can go missing.
        alias["AliasTarget"] = newName;
        Utils::SaveJsonToFile(alias, oldPath);
    }

    void SavePipelineProfile(const PipelineProfile& profile, bool bOverwrite)
    {
        ValidateName(profile.Name);
        (void)BuildPipelineProfilePatch(profile);
        const auto path = PipelineProfileDirectory() / (profile.Name + ".hepipeline");
        if (!bOverwrite && std::filesystem::exists(path)) throw std::runtime_error("A Profile with this name already exists.");
        std::filesystem::create_directories(path.parent_path());
        Utils::SaveJsonToFile(SerializePipelineProfile(profile), path);
    }
}
