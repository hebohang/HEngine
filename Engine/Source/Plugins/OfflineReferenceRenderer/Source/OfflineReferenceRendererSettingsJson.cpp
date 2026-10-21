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
            { "Schema", "hengine.offline-reference-renderer-settings.v2" },
            { "Interactive", settings.bInteractive },
            { "Denoise", settings.bDenoise },
            { "DenoiseIntervalMilliseconds", settings.DenoiseIntervalMilliseconds },
            { "TargetFramesPerSecond", settings.TargetFramesPerSecond },
            { "WorkBudgetMilliseconds", settings.WorkBudgetMilliseconds },
            { "InteractiveScale", settings.InteractiveScale },
            { "SettleMilliseconds", settings.SettleMilliseconds },
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
        // v1 batch counts remain persisted as a legacy preference; neither
        // strategy may bypass the bounded scheduler with a large batch.
        result.bInteractive = json.value("Interactive", defaults.bInteractive);
        result.bDenoise = json.value("Denoise", defaults.bDenoise);
        result.DenoiseIntervalMilliseconds = std::clamp(json.value("DenoiseIntervalMilliseconds", defaults.DenoiseIntervalMilliseconds), 100u, 10000u);
        result.TargetFramesPerSecond = std::clamp(json.value("TargetFramesPerSecond", defaults.TargetFramesPerSecond), 15.0f, 120.0f);
        result.WorkBudgetMilliseconds = std::clamp(json.value("WorkBudgetMilliseconds", defaults.WorkBudgetMilliseconds), 0.25f, 16.0f);
        result.InteractiveScale = std::clamp(json.value("InteractiveScale", defaults.InteractiveScale), 0.05f, 1.0f);
        result.SettleMilliseconds = std::clamp(json.value("SettleMilliseconds", defaults.SettleMilliseconds), 100.0f, 2000.0f);
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
        if (previewAov <= static_cast<uint32_t>(OfflineViewportAov::DenoisedBeauty))
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
