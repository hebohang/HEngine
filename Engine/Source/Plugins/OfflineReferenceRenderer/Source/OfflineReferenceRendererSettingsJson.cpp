#include "Hepch.h"

#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSettingsJson.h"

#include <algorithm>
#include <string>

namespace HEngine
{
    Json SerializeOfflineReferenceRendererSettings(
        const OfflineViewportRendererSettings& settings)
    {
        return Json{
            { "Schema", "hengine.offline-reference-renderer-settings.v1" },
            { "TargetSamples", settings.TargetSamples },
            { "SamplesPerFrame", settings.SamplesPerFrame },
            { "MaxBounces", settings.MaxBounces },
            { "MaximumRayDistance", settings.MaximumRayDistance },
            { "RenderScale", settings.RenderScale },
            { "FixedWidth", settings.FixedWidth },
            { "FixedHeight", settings.FixedHeight },
            { "LockAspectRatio", settings.bLockAspectRatio },
            { "SeedMode", settings.SeedMode == OfflineViewportSeedMode::Fixed
                ? "Fixed" : "Random" },
            { "Seed", settings.Seed },
            { "EnvironmentRadiance", {
                settings.EnvironmentRadiance.x,
                settings.EnvironmentRadiance.y,
                settings.EnvironmentRadiance.z } },
            { "UseSceneEnvironment", settings.bUseSceneEnvironment },
            { "DirectOnly", settings.bDirectOnly },
            { "AutoReset", settings.bAutoReset },
            { "PreviewAov", static_cast<uint32_t>(settings.PreviewAov) },
            { "OutputDirectory", settings.OutputDirectory.string() },
            { "OutputFileName", settings.OutputFileName },
            { "OutputFormat", static_cast<uint32_t>(settings.OutputFormat) },
        };
    }

    OfflineViewportRendererSettings DeserializeOfflineReferenceRendererSettings(
        const Json& json,
        const OfflineViewportRendererSettings& defaults)
    {
        if (!json.is_object())
            return defaults;
        OfflineViewportRendererSettings result = defaults;
        result.TargetSamples = std::clamp(
            json.value("TargetSamples", defaults.TargetSamples),
            1u,
            1048576u);
        result.SamplesPerFrame = std::clamp(
            json.value("SamplesPerFrame", defaults.SamplesPerFrame),
            1u,
            1024u);
        result.MaxBounces = std::min(
            json.value("MaxBounces", defaults.MaxBounces),
            16u);
        result.MaximumRayDistance = std::max(
            json.value("MaximumRayDistance", defaults.MaximumRayDistance),
            0.001f);
        result.RenderScale = std::clamp(
            json.value("RenderScale", defaults.RenderScale),
            0.01f,
            8.0f);
        result.FixedWidth = std::min(
            json.value("FixedWidth", defaults.FixedWidth),
            16384u);
        result.FixedHeight = std::min(
            json.value("FixedHeight", defaults.FixedHeight),
            16384u);
        result.bLockAspectRatio = json.value(
            "LockAspectRatio",
            defaults.bLockAspectRatio);
        const std::string seedMode = json.value(
            "SeedMode",
            defaults.SeedMode == OfflineViewportSeedMode::Fixed
                ? std::string("Fixed")
                : std::string("Random"));
        result.SeedMode = seedMode == "Random"
            ? OfflineViewportSeedMode::Random
            : OfflineViewportSeedMode::Fixed;
        result.Seed = json.value("Seed", defaults.Seed);
        if (json.contains("EnvironmentRadiance") &&
            json["EnvironmentRadiance"].is_array() &&
            json["EnvironmentRadiance"].size() == 3)
        {
            result.EnvironmentRadiance = glm::vec3(
                json["EnvironmentRadiance"][0].get<float>(),
                json["EnvironmentRadiance"][1].get<float>(),
                json["EnvironmentRadiance"][2].get<float>());
        }
        result.bUseSceneEnvironment = json.value(
            "UseSceneEnvironment",
            defaults.bUseSceneEnvironment);
        result.bDirectOnly = json.value("DirectOnly", defaults.bDirectOnly);
        result.bAutoReset = json.value("AutoReset", defaults.bAutoReset);
        const uint32_t previewAov = json.value(
            "PreviewAov",
            static_cast<uint32_t>(defaults.PreviewAov));
        if (previewAov <= static_cast<uint32_t>(OfflineViewportAov::SampleCount))
            result.PreviewAov = static_cast<OfflineViewportAov>(previewAov);
        result.OutputDirectory = std::filesystem::path(json.value(
            "OutputDirectory",
            defaults.OutputDirectory.string()));
        result.OutputFileName = json.value(
            "OutputFileName",
            defaults.OutputFileName);
        const uint32_t outputFormat = json.value(
            "OutputFormat",
            static_cast<uint32_t>(defaults.OutputFormat));
        if (outputFormat <=
            static_cast<uint32_t>(OfflineViewportOutputFormat::Raw))
        {
            result.OutputFormat =
                static_cast<OfflineViewportOutputFormat>(outputFormat);
        }
        return result;
    }
}
