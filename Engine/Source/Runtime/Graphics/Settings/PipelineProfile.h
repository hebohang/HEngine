#pragma once

#include "Runtime/Graphics/Settings/RenderSettings.h"

#include <filesystem>

namespace HEngine
{
    // Profiles are portable snapshots expanded into the existing settings transaction.
    // Unknown plugin payloads remain intact so restoring a plugin restores its settings.
    struct PipelineProfile
    {
        std::string Name = "Default";
        Json Values = Json::object();
    };

    [[nodiscard]] Json SerializePipelineProfile(const PipelineProfile& profile);
    [[nodiscard]] PipelineProfile ParsePipelineProfile(const Json& document);
    [[nodiscard]] PipelineProfile CapturePipelineProfile(std::string name, const RenderSettings& settings);
    [[nodiscard]] PipelineProfile MakeBuiltinPipelineProfile(std::string_view name);
    [[nodiscard]] Json BuildPipelineProfilePatch(const PipelineProfile& profile);
    [[nodiscard]] std::filesystem::path PipelineProfileDirectory();
    [[nodiscard]] std::vector<std::string> ListPipelineProfiles();
    [[nodiscard]] PipelineProfile LoadPipelineProfile(std::string_view name);
    [[nodiscard]] PipelineProfile LoadPipelineProfileAsset(const std::filesystem::path& path);
    void RenamePipelineProfile(std::string_view oldName, std::string_view newName);
    void SavePipelineProfile(const PipelineProfile& profile, bool bOverwrite = false);
}
