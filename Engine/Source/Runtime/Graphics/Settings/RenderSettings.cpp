#include "Hepch.h"

#include "Runtime/Graphics/Settings/RenderSettings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace HEngine
{
    using namespace std::string_view_literals;

    namespace
    {
        using Preset = GlobalIlluminationQualityPreset;

        [[nodiscard]] std::vector<SettingsLayer> RenderLayers()
        {
            return { SettingsLayer::Project, SettingsLayer::User,
                SettingsLayer::Session, SettingsLayer::World,
                SettingsLayer::Camera };
        }

        [[nodiscard]] RenderSettingDescriptor Descriptor(
            std::string_view id,
            RenderSettingValueType type,
            Json defaultValue,
            std::string_view category,
            std::string_view subcategory,
            std::string_view label,
            ApplyImpact impact,
            std::optional<double> minimum = {},
            std::optional<double> maximum = {},
            bool bAdvanced = false,
            std::vector<std::string> enumValues = {},
            std::string_view capability = {})
        {
            std::string textId = "Editor.Settings." + std::string(id);
            return {
                .PropertyId = std::string(id),
                .Type = type,
                .DefaultValue = std::move(defaultValue),
                .Minimum = minimum,
                .Maximum = maximum,
                .Category = std::string(category),
                .Subcategory = std::string(subcategory),
                .LabelTextId = textId + ".Label",
                .DescriptionTextId = textId + ".Description",
                .EnglishFallback = std::string(label),
                .WritableLayers = RenderLayers(),
                .EnumValues = std::move(enumValues),
                .CapabilityPredicate = std::string(capability),
                .Impact = impact,
                .bAdvanced = bAdvanced,
            };
        }

        [[nodiscard]] std::vector<RenderSettingDescriptor> BuildDescriptors()
        {
            const RenderSettings defaults = MakeEmergencyFallbackRenderSettings();
            std::vector<RenderSettingDescriptor> result;
            result.reserve(34);
            result.push_back(Descriptor(RenderSettingIds::Backend,
                RenderSettingValueType::Enumeration, ToString(defaults.Backend),
                "Engine", "WindowAndDisplay", "Renderer backend",
                ApplyImpact::ProcessRestart, {}, {}, true,
                { "DX12", "Vulkan" }, "Rendering.Backend.Supported"));
            result.back().bRestartRequired = true;
            result.back().WritableLayers = {
                SettingsLayer::Project,
                SettingsLayer::User,
            };
            result.push_back(Descriptor(RenderSettingIds::VSync,
                RenderSettingValueType::Boolean, defaults.bVSync,
                "Engine", "WindowAndDisplay", "Vertical synchronization",
                ApplyImpact::Live));
            result.push_back(Descriptor(RenderSettingIds::AntiAliasingMode,
                RenderSettingValueType::Enumeration,
                ToString(defaults.AntiAliasing.Mode), "Rendering",
                "AntiAliasing", "Anti-aliasing mode",
                ApplyImpact::RenderTargetRecreate, {}, {}, false,
                { "None", "FXAA", "SMAA", "TAA", "MSAA" },
                "Rendering.AntiAliasing.Mode.Available"));
            result.push_back(Descriptor(RenderSettingIds::MsaaSamples,
                RenderSettingValueType::Integer,
                defaults.AntiAliasing.MsaaSamples, "Rendering",
                "AntiAliasing", "MSAA samples",
                ApplyImpact::RenderTargetRecreate, 2.0, 8.0));
            result.push_back(Descriptor(RenderSettingIds::FxaaEdgeThreshold,
                RenderSettingValueType::Float,
                defaults.AntiAliasing.FxaaEdgeThreshold, "Rendering",
                "AntiAliasing", "FXAA edge threshold", ApplyImpact::Live,
                0.0001, 1.0, true));
            result.push_back(Descriptor(RenderSettingIds::FxaaEdgeThresholdMin,
                RenderSettingValueType::Float,
                defaults.AntiAliasing.FxaaEdgeThresholdMin, "Rendering",
                "AntiAliasing", "FXAA minimum edge threshold", ApplyImpact::Live,
                0.0, 1.0, true));
            result.push_back(Descriptor(RenderSettingIds::FxaaSubpixelQuality,
                RenderSettingValueType::Float,
                defaults.AntiAliasing.FxaaSubpixelQuality, "Rendering",
                "AntiAliasing", "FXAA subpixel quality", ApplyImpact::Live,
                0.0, 1.0, true));
            result.push_back(Descriptor(RenderSettingIds::SmaaEdgeThreshold,
                RenderSettingValueType::Float,
                defaults.AntiAliasing.SmaaEdgeThreshold, "Rendering",
                "AntiAliasing", "SMAA edge threshold", ApplyImpact::Live,
                0.0001, 1.0, true));
            result.push_back(Descriptor(RenderSettingIds::SmaaMaxSearchSteps,
                RenderSettingValueType::Integer,
                defaults.AntiAliasing.SmaaMaxSearchSteps, "Rendering",
                "AntiAliasing", "SMAA maximum search steps", ApplyImpact::Live,
                1.0, 32.0, true));
            result.push_back(Descriptor(
                RenderSettingIds::SmaaMaxDiagonalSearchSteps,
                RenderSettingValueType::Integer,
                defaults.AntiAliasing.SmaaMaxDiagonalSearchSteps, "Rendering",
                "AntiAliasing", "SMAA diagonal search steps", ApplyImpact::Live,
                0.0, 16.0, true));
            result.push_back(Descriptor(RenderSettingIds::SmaaCornerRounding,
                RenderSettingValueType::Float,
                defaults.AntiAliasing.SmaaCornerRounding, "Rendering",
                "AntiAliasing", "SMAA corner rounding", ApplyImpact::Live,
                0.0, 1.0, true));
            result.push_back(Descriptor(RenderSettingIds::TaaHistoryWeight,
                RenderSettingValueType::Float,
                defaults.AntiAliasing.TaaHistoryWeight, "Rendering",
                "AntiAliasing", "TAA history weight",
                ApplyImpact::TemporalHistoryReset, 0.0, 0.999, true));
            result.push_back(Descriptor(RenderSettingIds::TaaSharpness,
                RenderSettingValueType::Float,
                defaults.AntiAliasing.TaaSharpness, "Rendering",
                "AntiAliasing", "TAA sharpness", ApplyImpact::Live,
                0.0, 1.0, true));

            result.push_back(Descriptor(RenderSettingIds::GiTechnique,
                RenderSettingValueType::Enumeration, "Off", "Rendering",
                "GlobalIllumination.Common", "GI technique",
                ApplyImpact::RenderGraphRebuild, {}, {}, false,
                { "Off", "ScreenSpaceGI", "HardwareRTGI",
                    "ExternalViewportRenderer" },
                "Rendering.GI.Technique.Available"));
            result.push_back(Descriptor(RenderSettingIds::GiDenoiser,
                RenderSettingValueType::Enumeration, "SVGF", "Rendering",
                "GlobalIllumination.Denoising", "GI denoiser",
                ApplyImpact::RenderGraphRebuild, {}, {}, false,
                { "None", "SVGF", "NRD" },
                "Rendering.GI.Denoiser.Available"));
            result.push_back(Descriptor(RenderSettingIds::GiUnsupportedFallback,
                RenderSettingValueType::Enumeration, "Off", "Rendering",
                "GlobalIllumination.Common", "Unsupported GI fallback",
                ApplyImpact::RenderGraphRebuild, {}, {}, true,
                { "Off", "ScreenSpaceGI" }));
            result.push_back(Descriptor(RenderSettingIds::GiRenderScale,
                RenderSettingValueType::Float,
                defaults.GlobalIllumination.RenderScale, "Rendering",
                "GlobalIllumination.Common", "GI render scale",
                ApplyImpact::RenderTargetRecreate, 0.5, 1.0));
            result.back().Unit = "ratio";
            result.push_back(Descriptor(RenderSettingIds::GiSamplesPerPixel,
                RenderSettingValueType::Integer,
                defaults.GlobalIllumination.SamplesPerPixel, "Rendering",
                "GlobalIllumination.HardwareRTGI", "GI samples per pixel",
                ApplyImpact::TemporalHistoryReset, 1.0, 16.0));
            result.push_back(Descriptor(RenderSettingIds::GiMaxBounces,
                RenderSettingValueType::Integer,
                defaults.GlobalIllumination.MaxBounces, "Rendering",
                "GlobalIllumination.HardwareRTGI", "GI maximum bounces",
                ApplyImpact::TemporalHistoryReset, 1.0, 8.0));
            result.push_back(Descriptor(RenderSettingIds::GiAtrousIterations,
                RenderSettingValueType::Integer,
                defaults.GlobalIllumination.AtrousIterations, "Rendering",
                "GlobalIllumination.Denoising", "A-trous iterations",
                ApplyImpact::RenderGraphRebuild, 1.0, 5.0));
            result.push_back(Descriptor(RenderSettingIds::GiBilateralUpsample,
                RenderSettingValueType::Boolean,
                defaults.GlobalIllumination.bBilateralUpsample, "Rendering",
                "GlobalIllumination.Denoising", "Bilateral upsample",
                ApplyImpact::RenderGraphRebuild, {}, {}, true));
            result.push_back(Descriptor(RenderSettingIds::GiSsgiRayCount,
                RenderSettingValueType::Integer,
                defaults.GlobalIllumination.ScreenSpaceRayCount, "Rendering",
                "GlobalIllumination.ScreenSpaceGI", "SSGI ray count",
                ApplyImpact::TemporalHistoryReset, 1.0, 8.0));
            result.push_back(Descriptor(RenderSettingIds::GiSsgiStepCount,
                RenderSettingValueType::Integer,
                defaults.GlobalIllumination.ScreenSpaceStepCount, "Rendering",
                "GlobalIllumination.ScreenSpaceGI", "SSGI step count",
                ApplyImpact::TemporalHistoryReset, 1.0, 256.0));
            result.push_back(Descriptor(RenderSettingIds::GiSsgiHiZStartMip,
                RenderSettingValueType::Integer,
                defaults.GlobalIllumination.ScreenSpaceHiZStartMip, "Rendering",
                "GlobalIllumination.ScreenSpaceGI", "SSGI Hi-Z start mip",
                ApplyImpact::TemporalHistoryReset, 0.0, 16.0, true));
            result.push_back(Descriptor(
                RenderSettingIds::GiSsgiBinaryRefinementSteps,
                RenderSettingValueType::Integer,
                defaults.GlobalIllumination.ScreenSpaceBinaryRefinementSteps,
                "Rendering", "GlobalIllumination.ScreenSpaceGI",
                "SSGI binary refinement steps",
                ApplyImpact::TemporalHistoryReset, 0.0, 8.0, true));
            result.push_back(Descriptor(
                RenderSettingIds::GiSsgiMaximumRayDistance,
                RenderSettingValueType::Float,
                defaults.GlobalIllumination.ScreenSpaceMaximumRayDistance,
                "Rendering", "GlobalIllumination.ScreenSpaceGI",
                "SSGI maximum ray distance",
                ApplyImpact::TemporalHistoryReset, 0.001, 10000.0, true));
            result.back().Unit = "world-units";
            result.push_back(Descriptor(RenderSettingIds::GiSsgiThickness,
                RenderSettingValueType::Float,
                defaults.GlobalIllumination.ScreenSpaceThickness, "Rendering",
                "GlobalIllumination.ScreenSpaceGI", "SSGI thickness",
                ApplyImpact::TemporalHistoryReset, 0.0001, 100.0, true));
            result.push_back(Descriptor(RenderSettingIds::GiSsgiEdgeFade,
                RenderSettingValueType::Float,
                defaults.GlobalIllumination.ScreenSpaceEdgeFade, "Rendering",
                "GlobalIllumination.ScreenSpaceGI", "SSGI edge fade",
                ApplyImpact::Live, 0.0, 0.5, true));
            result.push_back(Descriptor(RenderSettingIds::GiSsgiTemporalWeight,
                RenderSettingValueType::Float,
                defaults.GlobalIllumination.ScreenSpaceTemporalWeight,
                "Rendering", "GlobalIllumination.ScreenSpaceGI",
                "SSGI temporal weight", ApplyImpact::TemporalHistoryReset,
                0.0, 0.999, true));
            result.push_back(Descriptor(RenderSettingIds::PostProcessExposure,
                RenderSettingValueType::Float, defaults.PostProcess.Exposure,
                "Rendering", "PostProcess", "Default exposure",
                ApplyImpact::Live, 0.0, 10.0));
            result.push_back(Descriptor(RenderSettingIds::ShadowsEnabled,
                RenderSettingValueType::Boolean, defaults.Shadows.bEnabled,
                "Rendering", "Shadows", "Enable dynamic shadows",
                ApplyImpact::RenderGraphRebuild));

            auto debugView = Descriptor(RenderSettingIds::GiDebugView,
                RenderSettingValueType::Integer, 0, "Rendering",
                "GlobalIllumination.Diagnostics", "GI debug view",
                ApplyImpact::RenderGraphRebuild, 0.0, 14.0, true);
            debugView.WritableLayers = {
                SettingsLayer::User, SettingsLayer::Session };
            debugView.bPersistedRenderSetting = false;
            result.push_back(std::move(debugView));
            auto directOnly = Descriptor(RenderSettingIds::GiDirectOnly,
                RenderSettingValueType::Boolean, false, "Rendering",
                "GlobalIllumination.Diagnostics", "Direct lighting only",
                ApplyImpact::RenderGraphRebuild, {}, {}, true);
            directOnly.WritableLayers = {
                SettingsLayer::User, SettingsLayer::Session };
            directOnly.bPersistedRenderSetting = false;
            result.push_back(std::move(directOnly));
            auto pbrDebug = Descriptor(RenderSettingIds::PbrDebugMode,
                RenderSettingValueType::Integer, 0, "Rendering", "Diagnostics",
                "PBR debug mode", ApplyImpact::Live, 0.0, 11.0, true);
            pbrDebug.WritableLayers = {
                SettingsLayer::User, SettingsLayer::Session };
            pbrDebug.bPersistedRenderSetting = false;
            result.push_back(std::move(pbrDebug));
            auto graphValidation = Descriptor(
                RenderSettingIds::RenderGraphValidation,
                RenderSettingValueType::Boolean, false, "Rendering",
                "Diagnostics", "RenderGraph validation",
                ApplyImpact::ProcessRestart, {}, {}, true);
            graphValidation.bRestartRequired = true;
            graphValidation.WritableLayers = {
                SettingsLayer::User, SettingsLayer::Session };
            graphValidation.bPersistedRenderSetting = false;
            result.push_back(std::move(graphValidation));
            auto shadowDebug = Descriptor(RenderSettingIds::ShadowCascadeDebug,
                RenderSettingValueType::Boolean, false, "Rendering",
                "Diagnostics", "Shadow cascade coverage",
                ApplyImpact::Live, {}, {}, true);
            shadowDebug.WritableLayers = {
                SettingsLayer::User, SettingsLayer::Session };
            shadowDebug.bPersistedRenderSetting = false;
            result.push_back(std::move(shadowDebug));
            auto capture = Descriptor(RenderSettingIds::CaptureSupersampleScale,
                RenderSettingValueType::Integer, 1, "Rendering", "Capture",
                "Capture supersample scale", ApplyImpact::Live,
                1.0, 4.0, true);
            capture.WritableLayers = {
                SettingsLayer::User, SettingsLayer::Session };
            capture.bPersistedRenderSetting = false;
            result.push_back(std::move(capture));

            const std::array presetIds = {
                RenderSettingIds::GiRenderScale,
                RenderSettingIds::GiMaxBounces,
                RenderSettingIds::GiAtrousIterations,
                RenderSettingIds::GiSsgiRayCount,
                RenderSettingIds::GiSsgiStepCount,
            };
            for (RenderSettingDescriptor& descriptor : result)
            {
                if (std::ranges::find(presetIds, descriptor.PropertyId) !=
                    presetIds.end())
                {
                    descriptor.PresetMembership = {
                        Preset::Low, Preset::Balanced, Preset::High };
                }
            }
            return result;
        }

        [[nodiscard]] const char* GiTechniqueName(
            GlobalIlluminationTechniqueKind technique) noexcept
        {
            switch (technique)
            {
            case GlobalIlluminationTechniqueKind::Off: return "Off";
            case GlobalIlluminationTechniqueKind::ScreenSpace:
                return "ScreenSpaceGI";
            case GlobalIlluminationTechniqueKind::HardwareRayTracing:
                return "HardwareRTGI";
            case GlobalIlluminationTechniqueKind::ExternalViewportRenderer:
                return "ExternalViewportRenderer";
            }
            return "Off";
        }

        [[nodiscard]] const char* GiDenoiserName(
            GlobalIlluminationDenoiserKind denoiser) noexcept
        {
            switch (denoiser)
            {
            case GlobalIlluminationDenoiserKind::None: return "None";
            case GlobalIlluminationDenoiserKind::Svgf: return "SVGF";
            case GlobalIlluminationDenoiserKind::Nrd: return "NRD";
            }
            return "None";
        }

        [[nodiscard]] const char* GiFallbackName(
            GlobalIlluminationFallbackKind fallback) noexcept
        {
            return fallback == GlobalIlluminationFallbackKind::ScreenSpace
                ? "ScreenSpaceGI" : "Off";
        }

        void AddDiagnostic(
            std::vector<RenderSettingDiagnostic>& diagnostics,
            std::string_view id,
            Json input,
            Json normalized,
            RenderSettingDiagnosticSeverity severity,
            std::string_view code,
            std::string message)
        {
            diagnostics.push_back({ std::string(id), std::move(input),
                std::move(normalized), severity, std::string(code),
                std::move(message) });
        }

        [[nodiscard]] Json NormalizeValue(
            const RenderSettingDescriptor& descriptor,
            const Json& input,
            std::vector<RenderSettingDiagnostic>& diagnostics)
        {
            const auto fail = [&](std::string message)
            {
                AddDiagnostic(diagnostics, descriptor.PropertyId, input, {},
                    RenderSettingDiagnosticSeverity::Error, "invalid_type",
                    std::move(message));
                return Json{};
            };
            switch (descriptor.Type)
            {
            case RenderSettingValueType::Boolean:
                if (!input.is_boolean())
                    return fail("Expected a boolean value.");
                return input;
            case RenderSettingValueType::Integer:
            {
                if (!input.is_number_integer())
                    return fail("Expected an integer value.");
                const double number = input.get<double>();
                double normalized = number;
                if (descriptor.Minimum)
                    normalized = std::max(normalized, *descriptor.Minimum);
                if (descriptor.Maximum)
                    normalized = std::min(normalized, *descriptor.Maximum);
                const std::int64_t integer = static_cast<std::int64_t>(normalized);
                if (normalized != number)
                {
                    AddDiagnostic(diagnostics, descriptor.PropertyId, input,
                        integer, RenderSettingDiagnosticSeverity::Warning,
                        "normalized_range", "The value was outside the supported range and was normalized.");
                }
                return integer;
            }
            case RenderSettingValueType::Float:
            {
                if (!input.is_number())
                    return fail("Expected a numeric value.");
                const double number = input.get<double>();
                if (!std::isfinite(number))
                    return fail("Expected a finite numeric value.");
                double normalized = number;
                if (descriptor.Minimum)
                    normalized = std::max(normalized, *descriptor.Minimum);
                if (descriptor.Maximum)
                    normalized = std::min(normalized, *descriptor.Maximum);
                if (normalized != number)
                {
                    AddDiagnostic(diagnostics, descriptor.PropertyId, input,
                        normalized, RenderSettingDiagnosticSeverity::Warning,
                        "normalized_range", "The value was outside the supported range and was normalized.");
                }
                return normalized;
            }
            case RenderSettingValueType::Enumeration:
                if (!input.is_string())
                    return fail("Expected a canonical enumeration string.");
                if (std::ranges::find(descriptor.EnumValues,
                        input.get_ref<const std::string&>()) ==
                    descriptor.EnumValues.end())
                {
                    AddDiagnostic(diagnostics, descriptor.PropertyId, input, {},
                        RenderSettingDiagnosticSeverity::Error,
                        "unknown_enum_value",
                        "The value is not a supported canonical enumeration ID.");
                    return {};
                }
                return input;
            case RenderSettingValueType::String:
                if (!input.is_string())
                    return fail("Expected a string value.");
                return input;
            }
            return {};
        }

        void SetPath(Json& root, std::string_view dottedPath, const Json& value)
        {
            Json* current = &root;
            std::size_t start = 0;
            while (start < dottedPath.size())
            {
                const std::size_t end = dottedPath.find('.', start);
                const std::string key(dottedPath.substr(start,
                    end == std::string_view::npos
                        ? dottedPath.size() - start : end - start));
                if (end == std::string_view::npos)
                {
                    (*current)[key] = value;
                    return;
                }
                current = &(*current)[key];
                start = end + 1;
            }
        }

        [[nodiscard]] const Json* FindPath(
            const Json& root,
            std::string_view dottedPath)
        {
            const Json* current = &root;
            std::size_t start = 0;
            while (start < dottedPath.size())
            {
                const std::size_t end = dottedPath.find('.', start);
                const std::string key(dottedPath.substr(start,
                    end == std::string_view::npos
                        ? dottedPath.size() - start : end - start));
                if (!current->is_object())
                    return nullptr;
                const auto iterator = current->find(key);
                if (iterator == current->end())
                    return nullptr;
                current = &*iterator;
                if (end == std::string_view::npos)
                    return current;
                start = end + 1;
            }
            return nullptr;
        }

        void CopyIfPresent(
            Json& destination,
            std::string_view destinationId,
            const Json& source,
            std::string_view sourcePath)
        {
            if (const Json* value = FindPath(source, sourcePath))
                destination[std::string(destinationId)] = *value;
        }

        [[nodiscard]] GlobalIlluminationTechniqueKind ParseGiTechnique(
            std::string_view name)
        {
            if (name == "Off") return GlobalIlluminationTechniqueKind::Off;
            if (name == "ScreenSpaceGI")
                return GlobalIlluminationTechniqueKind::ScreenSpace;
            if (name == "HardwareRTGI")
                return GlobalIlluminationTechniqueKind::HardwareRayTracing;
            return GlobalIlluminationTechniqueKind::ExternalViewportRenderer;
        }

        [[nodiscard]] GlobalIlluminationDenoiserKind ParseGiDenoiser(
            std::string_view name)
        {
            if (name == "None") return GlobalIlluminationDenoiserKind::None;
            if (name == "NRD") return GlobalIlluminationDenoiserKind::Nrd;
            return GlobalIlluminationDenoiserKind::Svgf;
        }

        [[nodiscard]] bool IsToolProperty(std::string_view id) noexcept
        {
            return id == RenderSettingIds::GiDebugView ||
                id == RenderSettingIds::GiDirectOnly ||
                id == RenderSettingIds::PbrDebugMode ||
                id == RenderSettingIds::RenderGraphValidation ||
                id == RenderSettingIds::ShadowCascadeDebug ||
                id == RenderSettingIds::CaptureSupersampleScale;
        }

        [[nodiscard]] bool HasAaCapabilities(
            const AntiAliasingCapabilities& capabilities) noexcept
        {
            return capabilities.Fxaa.bAvailable ||
                capabilities.Smaa.bAvailable ||
                capabilities.Taa.bAvailable ||
                capabilities.Msaa2x.bAvailable ||
                capabilities.Msaa4x.bAvailable ||
                capabilities.Msaa8x.bAvailable;
        }
    }

    const char* ToString(SettingsLayer layer) noexcept
    {
        switch (layer)
        {
        case SettingsLayer::EmergencyFallback: return "EmergencyFallback";
        case SettingsLayer::EngineDefault: return "Engine";
        case SettingsLayer::Project: return "Project";
        case SettingsLayer::User: return "User";
        case SettingsLayer::Session: return "Session";
        case SettingsLayer::CommandLine: return "CommandLine";
        case SettingsLayer::World: return "World";
        case SettingsLayer::Camera: return "Camera";
        case SettingsLayer::Capability: return "Capability";
        case SettingsLayer::Effective: return "Effective";
        }
        return "Unknown";
    }

    const char* ToString(ApplyImpact impact) noexcept
    {
        switch (impact)
        {
        case ApplyImpact::Live: return "Live";
        case ApplyImpact::TemporalHistoryReset: return "TemporalHistoryReset";
        case ApplyImpact::RenderGraphRebuild: return "RenderGraphRebuild";
        case ApplyImpact::RenderTargetRecreate: return "RenderTargetRecreate";
        case ApplyImpact::SwapchainRecreate: return "SwapchainRecreate";
        case ApplyImpact::ProcessRestart: return "ProcessRestart";
        case ApplyImpact::BuildReconfigure: return "BuildReconfigure";
        }
        return "Unknown";
    }

    const char* ToString(RenderSettingDiagnosticSeverity severity) noexcept
    {
        switch (severity)
        {
        case RenderSettingDiagnosticSeverity::Info: return "Info";
        case RenderSettingDiagnosticSeverity::Warning: return "Warning";
        case RenderSettingDiagnosticSeverity::Error: return "Error";
        }
        return "Unknown";
    }

    const char* ToString(GlobalIlluminationQualityPreset preset) noexcept
    {
        switch (preset)
        {
        case Preset::Custom: return "Custom";
        case Preset::Low: return "Low";
        case Preset::Balanced: return "Balanced";
        case Preset::High: return "High";
        }
        return "Custom";
    }

    RenderSettings MakeEmergencyFallbackRenderSettings()
    {
        RenderSettings settings;
        settings.Backend = RenderBackend::DX12;
        settings.bVSync = false;
        // Keep the emergency fallback aligned with the shipped product
        // defaults. A unit contract compares this complete value against
        // Engine/Content/Config/DefaultEngine.json.
        settings.GlobalIllumination.ScreenSpaceRayCount = 8;
        settings.GlobalIllumination.ScreenSpaceStepCount = 128;
        settings.GlobalIllumination.ScreenSpaceHiZStartMip = 0;
        settings.GlobalIllumination.ScreenSpaceBinaryRefinementSteps = 6;
        settings.GlobalIllumination.ScreenSpaceMaximumRayDistance = 1.0f;
        settings.GlobalIllumination.ScreenSpaceThickness = 0.015f;
        settings.GlobalIllumination.ScreenSpaceEdgeFade = 0.02f;
        settings.GlobalIllumination.ScreenSpaceTemporalWeight = 0.97f;
        return settings;
    }

    RenderDebugSettings ReadRenderDebugSettingsFromEnvironment()
    {
        RenderDebugSettings result;
#if !defined(HE_PACKAGED_GAME)
        const auto normalizedValue = [](const char* name)
        {
            const char* raw = std::getenv(name);
            if (!raw)
                return std::string{};
            std::string value(raw);
            std::ranges::transform(value, value.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
            return value;
        };
        const std::string pbr = normalizedValue("HE_PBR_DEBUG_VIEW");
        static const std::map<std::string, std::uint32_t, std::less<>> modes = {
            { "disabled", 0 }, { "0", 0 },
            { "basecolor", 1 }, { "1", 1 },
            { "worldnormal", 2 }, { "2", 2 },
            { "metallic", 3 }, { "3", 3 },
            { "roughness", 4 }, { "4", 4 },
            { "ambientocclusion", 5 }, { "ao", 5 }, { "5", 5 },
            { "directlighting", 6 }, { "6", 6 },
            { "diffuseibl", 7 }, { "7", 7 },
            { "specularibl", 8 }, { "8", 8 },
            { "shadow", 9 }, { "9", 9 },
            { "finalhdr", 10 }, { "10", 10 },
            { "hdrvalidity", 11 }, { "11", 11 },
        };
        if (!pbr.empty())
        {
            const auto iterator = modes.find(pbr);
            if (iterator != modes.end())
                result.PbrDebugMode = iterator->second;
            else
                HE_CORE_WARN("Ignoring unknown HE_PBR_DEBUG_VIEW value '{}'", pbr);
        }
        const std::string cascades =
            normalizedValue("HE_SHADOW_CASCADE_DEBUG");
        result.bShadowCascadeDebug = cascades == "1" ||
            cascades == "true" || cascades == "on" || cascades == "yes";
#endif
        return result;
    }

    bool RenderSettings::operator==(const RenderSettings& other) const noexcept
    {
        const AntiAliasingSettings& leftAa = AntiAliasing;
        const AntiAliasingSettings& rightAa = other.AntiAliasing;
        const GlobalIlluminationSettings& leftGi = GlobalIllumination;
        const GlobalIlluminationSettings& rightGi = other.GlobalIllumination;
        return Backend == other.Backend && bVSync == other.bVSync &&
            leftAa.Mode == rightAa.Mode &&
            leftAa.MsaaSamples == rightAa.MsaaSamples &&
            leftAa.FxaaEdgeThreshold == rightAa.FxaaEdgeThreshold &&
            leftAa.FxaaEdgeThresholdMin == rightAa.FxaaEdgeThresholdMin &&
            leftAa.FxaaSubpixelQuality == rightAa.FxaaSubpixelQuality &&
            leftAa.SmaaEdgeThreshold == rightAa.SmaaEdgeThreshold &&
            leftAa.SmaaMaxSearchSteps == rightAa.SmaaMaxSearchSteps &&
            leftAa.SmaaMaxDiagonalSearchSteps ==
                rightAa.SmaaMaxDiagonalSearchSteps &&
            leftAa.SmaaCornerRounding == rightAa.SmaaCornerRounding &&
            leftAa.TaaHistoryWeight == rightAa.TaaHistoryWeight &&
            leftAa.TaaSharpness == rightAa.TaaSharpness &&
            leftGi.Technique == rightGi.Technique &&
            leftGi.Denoiser == rightGi.Denoiser &&
            leftGi.UnsupportedFallback == rightGi.UnsupportedFallback &&
            leftGi.RenderScale == rightGi.RenderScale &&
            leftGi.SamplesPerPixel == rightGi.SamplesPerPixel &&
            leftGi.MaxBounces == rightGi.MaxBounces &&
            leftGi.AtrousIterations == rightGi.AtrousIterations &&
            leftGi.ScreenSpaceRayCount == rightGi.ScreenSpaceRayCount &&
            leftGi.ScreenSpaceStepCount == rightGi.ScreenSpaceStepCount &&
            leftGi.ScreenSpaceHiZStartMip == rightGi.ScreenSpaceHiZStartMip &&
            leftGi.ScreenSpaceBinaryRefinementSteps ==
                rightGi.ScreenSpaceBinaryRefinementSteps &&
            leftGi.ScreenSpaceMaximumRayDistance ==
                rightGi.ScreenSpaceMaximumRayDistance &&
            leftGi.ScreenSpaceThickness == rightGi.ScreenSpaceThickness &&
            leftGi.ScreenSpaceEdgeFade == rightGi.ScreenSpaceEdgeFade &&
            leftGi.ScreenSpaceTemporalWeight ==
                rightGi.ScreenSpaceTemporalWeight &&
            leftGi.bBilateralUpsample == rightGi.bBilateralUpsample &&
            PostProcess == other.PostProcess && Shadows == other.Shadows;
    }

    const std::vector<RenderSettingDescriptor>& GetRenderSettingDescriptors()
    {
        static const std::vector<RenderSettingDescriptor> descriptors =
            BuildDescriptors();
        return descriptors;
    }

    const RenderSettingDescriptor* FindRenderSettingDescriptor(
        std::string_view propertyId) noexcept
    {
        const auto& descriptors = GetRenderSettingDescriptors();
        const auto iterator = std::ranges::find(
            descriptors, propertyId, &RenderSettingDescriptor::PropertyId);
        return iterator == descriptors.end() ? nullptr : &*iterator;
    }

    Json RenderSettingsToFlatValues(const RenderSettings& settings)
    {
        return Json{
            { RenderSettingIds::Backend, ToString(settings.Backend) },
            { RenderSettingIds::VSync, settings.bVSync },
            { RenderSettingIds::AntiAliasingMode,
                ToString(settings.AntiAliasing.Mode) },
            { RenderSettingIds::MsaaSamples,
                settings.AntiAliasing.MsaaSamples },
            { RenderSettingIds::FxaaEdgeThreshold,
                settings.AntiAliasing.FxaaEdgeThreshold },
            { RenderSettingIds::FxaaEdgeThresholdMin,
                settings.AntiAliasing.FxaaEdgeThresholdMin },
            { RenderSettingIds::FxaaSubpixelQuality,
                settings.AntiAliasing.FxaaSubpixelQuality },
            { RenderSettingIds::SmaaEdgeThreshold,
                settings.AntiAliasing.SmaaEdgeThreshold },
            { RenderSettingIds::SmaaMaxSearchSteps,
                settings.AntiAliasing.SmaaMaxSearchSteps },
            { RenderSettingIds::SmaaMaxDiagonalSearchSteps,
                settings.AntiAliasing.SmaaMaxDiagonalSearchSteps },
            { RenderSettingIds::SmaaCornerRounding,
                settings.AntiAliasing.SmaaCornerRounding },
            { RenderSettingIds::TaaHistoryWeight,
                settings.AntiAliasing.TaaHistoryWeight },
            { RenderSettingIds::TaaSharpness,
                settings.AntiAliasing.TaaSharpness },
            { RenderSettingIds::GiTechnique,
                GiTechniqueName(settings.GlobalIllumination.Technique) },
            { RenderSettingIds::GiDenoiser,
                GiDenoiserName(settings.GlobalIllumination.Denoiser) },
            { RenderSettingIds::GiUnsupportedFallback,
                GiFallbackName(settings.GlobalIllumination.UnsupportedFallback) },
            { RenderSettingIds::GiRenderScale,
                settings.GlobalIllumination.RenderScale },
            { RenderSettingIds::GiSamplesPerPixel,
                settings.GlobalIllumination.SamplesPerPixel },
            { RenderSettingIds::GiMaxBounces,
                settings.GlobalIllumination.MaxBounces },
            { RenderSettingIds::GiAtrousIterations,
                settings.GlobalIllumination.AtrousIterations },
            { RenderSettingIds::GiBilateralUpsample,
                settings.GlobalIllumination.bBilateralUpsample },
            { RenderSettingIds::GiSsgiRayCount,
                settings.GlobalIllumination.ScreenSpaceRayCount },
            { RenderSettingIds::GiSsgiStepCount,
                settings.GlobalIllumination.ScreenSpaceStepCount },
            { RenderSettingIds::GiSsgiHiZStartMip,
                settings.GlobalIllumination.ScreenSpaceHiZStartMip },
            { RenderSettingIds::GiSsgiBinaryRefinementSteps,
                settings.GlobalIllumination.ScreenSpaceBinaryRefinementSteps },
            { RenderSettingIds::GiSsgiMaximumRayDistance,
                settings.GlobalIllumination.ScreenSpaceMaximumRayDistance },
            { RenderSettingIds::GiSsgiThickness,
                settings.GlobalIllumination.ScreenSpaceThickness },
            { RenderSettingIds::GiSsgiEdgeFade,
                settings.GlobalIllumination.ScreenSpaceEdgeFade },
            { RenderSettingIds::GiSsgiTemporalWeight,
                settings.GlobalIllumination.ScreenSpaceTemporalWeight },
            { RenderSettingIds::PostProcessExposure,
                settings.PostProcess.Exposure },
            { RenderSettingIds::ShadowsEnabled, settings.Shadows.bEnabled },
        };
    }

    Json RenderDebugSettingsToFlatValues(const RenderDebugSettings& settings)
    {
        return Json{
            { RenderSettingIds::GiDebugView,
                static_cast<std::uint32_t>(settings.GiDebugView) },
            { RenderSettingIds::GiDirectOnly, settings.bGiDirectOnly },
            { RenderSettingIds::PbrDebugMode, settings.PbrDebugMode },
            { RenderSettingIds::RenderGraphValidation,
                settings.bRenderGraphValidation },
            { RenderSettingIds::ShadowCascadeDebug,
                settings.bShadowCascadeDebug },
        };
    }

    Json RenderCaptureSettingsToFlatValues(const RenderCaptureSettings& settings)
    {
        return Json{
            { RenderSettingIds::CaptureSupersampleScale,
                settings.SupersampleScale },
        };
    }

    RenderSettings ApplyRenderSettingsFlatValues(
        const RenderSettings& base,
        const Json& values,
        std::vector<RenderSettingDiagnostic>& diagnostics)
    {
        RenderSettings result = base;
        if (!values.is_object())
        {
            AddDiagnostic(diagnostics, "Rendering", values, {},
                RenderSettingDiagnosticSeverity::Error, "invalid_patch",
                "Render settings values must be an object keyed by stable property ID.");
            return result;
        }
        for (const auto& [id, input] : values.items())
        {
            const RenderSettingDescriptor* descriptor =
                FindRenderSettingDescriptor(id);
            if (!descriptor || IsToolProperty(id))
            {
                AddDiagnostic(diagnostics, id, input, {},
                    RenderSettingDiagnosticSeverity::Error,
                    "unknown_property", "Unknown persisted render setting ID.");
                continue;
            }
            const Json value = NormalizeValue(*descriptor, input, diagnostics);
            if (value.is_null())
                continue;
            if (id == RenderSettingIds::Backend)
                result.Backend = ParseRenderBackend(value.get<std::string>());
            else if (id == RenderSettingIds::VSync)
                result.bVSync = value.get<bool>();
            else if (id == RenderSettingIds::AntiAliasingMode)
                result.AntiAliasing.Mode =
                    ParseAntiAliasingMode(value.get<std::string>());
            else if (id == RenderSettingIds::MsaaSamples)
            {
                const std::uint32_t samples = value.get<std::uint32_t>();
                if (!IsValidMsaaSampleCount(samples))
                {
                    AddDiagnostic(diagnostics, id, input,
                        result.AntiAliasing.MsaaSamples,
                        RenderSettingDiagnosticSeverity::Error,
                        "unsupported_sample_count",
                        "MSAA sample count must be exactly 2, 4, or 8.");
                }
                else
                    result.AntiAliasing.MsaaSamples = samples;
            }
            else if (id == RenderSettingIds::FxaaEdgeThreshold)
                result.AntiAliasing.FxaaEdgeThreshold = value.get<float>();
            else if (id == RenderSettingIds::FxaaEdgeThresholdMin)
                result.AntiAliasing.FxaaEdgeThresholdMin = value.get<float>();
            else if (id == RenderSettingIds::FxaaSubpixelQuality)
                result.AntiAliasing.FxaaSubpixelQuality = value.get<float>();
            else if (id == RenderSettingIds::SmaaEdgeThreshold)
                result.AntiAliasing.SmaaEdgeThreshold = value.get<float>();
            else if (id == RenderSettingIds::SmaaMaxSearchSteps)
                result.AntiAliasing.SmaaMaxSearchSteps = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::SmaaMaxDiagonalSearchSteps)
                result.AntiAliasing.SmaaMaxDiagonalSearchSteps = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::SmaaCornerRounding)
                result.AntiAliasing.SmaaCornerRounding = value.get<float>();
            else if (id == RenderSettingIds::TaaHistoryWeight)
                result.AntiAliasing.TaaHistoryWeight = value.get<float>();
            else if (id == RenderSettingIds::TaaSharpness)
                result.AntiAliasing.TaaSharpness = value.get<float>();
            else if (id == RenderSettingIds::GiTechnique)
                result.GlobalIllumination.Technique =
                    ParseGiTechnique(value.get_ref<const std::string&>());
            else if (id == RenderSettingIds::GiDenoiser)
                result.GlobalIllumination.Denoiser =
                    ParseGiDenoiser(value.get_ref<const std::string&>());
            else if (id == RenderSettingIds::GiUnsupportedFallback)
                result.GlobalIllumination.UnsupportedFallback =
                    value == "ScreenSpaceGI"
                    ? GlobalIlluminationFallbackKind::ScreenSpace
                    : GlobalIlluminationFallbackKind::Off;
            else if (id == RenderSettingIds::GiRenderScale)
                result.GlobalIllumination.RenderScale = value.get<float>();
            else if (id == RenderSettingIds::GiSamplesPerPixel)
                result.GlobalIllumination.SamplesPerPixel = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::GiMaxBounces)
                result.GlobalIllumination.MaxBounces = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::GiAtrousIterations)
                result.GlobalIllumination.AtrousIterations = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::GiBilateralUpsample)
                result.GlobalIllumination.bBilateralUpsample = value.get<bool>();
            else if (id == RenderSettingIds::GiSsgiRayCount)
                result.GlobalIllumination.ScreenSpaceRayCount = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::GiSsgiStepCount)
                result.GlobalIllumination.ScreenSpaceStepCount = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::GiSsgiHiZStartMip)
                result.GlobalIllumination.ScreenSpaceHiZStartMip = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::GiSsgiBinaryRefinementSteps)
                result.GlobalIllumination.ScreenSpaceBinaryRefinementSteps = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::GiSsgiMaximumRayDistance)
                result.GlobalIllumination.ScreenSpaceMaximumRayDistance = value.get<float>();
            else if (id == RenderSettingIds::GiSsgiThickness)
                result.GlobalIllumination.ScreenSpaceThickness = value.get<float>();
            else if (id == RenderSettingIds::GiSsgiEdgeFade)
                result.GlobalIllumination.ScreenSpaceEdgeFade = value.get<float>();
            else if (id == RenderSettingIds::GiSsgiTemporalWeight)
                result.GlobalIllumination.ScreenSpaceTemporalWeight = value.get<float>();
            else if (id == RenderSettingIds::PostProcessExposure)
                result.PostProcess.Exposure = value.get<float>();
            else if (id == RenderSettingIds::ShadowsEnabled)
                result.Shadows.bEnabled = value.get<bool>();
        }
        return result;
    }

    RenderDebugSettings ApplyRenderDebugFlatValues(
        const RenderDebugSettings& base,
        const Json& values,
        std::vector<RenderSettingDiagnostic>& diagnostics)
    {
        RenderDebugSettings result = base;
        if (!values.is_object())
            return result;
        for (const auto& [id, input] : values.items())
        {
            const RenderSettingDescriptor* descriptor =
                FindRenderSettingDescriptor(id);
            if (!descriptor || (id != RenderSettingIds::GiDebugView &&
                    id != RenderSettingIds::GiDirectOnly &&
                    id != RenderSettingIds::PbrDebugMode &&
                    id != RenderSettingIds::RenderGraphValidation &&
                    id != RenderSettingIds::ShadowCascadeDebug))
            {
                AddDiagnostic(diagnostics, id, input, {},
                    RenderSettingDiagnosticSeverity::Error,
                    "unknown_debug_property", "Unknown render debug setting ID.");
                continue;
            }
            const Json value = NormalizeValue(*descriptor, input, diagnostics);
            if (value.is_null())
                continue;
            if (id == RenderSettingIds::GiDebugView)
                result.GiDebugView = static_cast<GlobalIlluminationDebugView>(
                    value.get<std::uint32_t>());
            else if (id == RenderSettingIds::GiDirectOnly)
                result.bGiDirectOnly = value.get<bool>();
            else if (id == RenderSettingIds::PbrDebugMode)
                result.PbrDebugMode = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::RenderGraphValidation)
                result.bRenderGraphValidation = value.get<bool>();
            else
                result.bShadowCascadeDebug = value.get<bool>();
        }
        return result;
    }

    RenderCaptureSettings ApplyRenderCaptureFlatValues(
        const RenderCaptureSettings& base,
        const Json& values,
        std::vector<RenderSettingDiagnostic>& diagnostics)
    {
        RenderCaptureSettings result = base;
        if (!values.is_object())
            return result;
        for (const auto& [id, input] : values.items())
        {
            if (id != RenderSettingIds::CaptureSupersampleScale)
            {
                AddDiagnostic(diagnostics, id, input, {},
                    RenderSettingDiagnosticSeverity::Error,
                    "unknown_capture_property", "Unknown render capture setting ID.");
                continue;
            }
            const RenderSettingDescriptor& descriptor =
                *FindRenderSettingDescriptor(id);
            const Json value = NormalizeValue(descriptor, input, diagnostics);
            if (value.is_null())
                continue;
            const std::uint32_t scale = value.get<std::uint32_t>();
            if (scale != 1 && scale != 2 && scale != 4)
            {
                AddDiagnostic(diagnostics, id, input, 1,
                    RenderSettingDiagnosticSeverity::Warning,
                    "normalized_supported_value",
                    "Capture supersampling supports only 1x, 2x, or 4x; normalized to 1x.");
                result.SupersampleScale = 1;
            }
            else
                result.SupersampleScale = scale;
        }
        return result;
    }

    RenderSettingsMigrationResult MigrateRenderSettingsDocument(
        const Json& document)
    {
        RenderSettingsMigrationResult result;
        if (!document.is_object())
        {
            result.bSuccess = false;
            AddDiagnostic(result.Diagnostics, "Schema", document, {},
                RenderSettingDiagnosticSeverity::Error, "invalid_document",
                "Render settings document must be a JSON object.");
            return result;
        }

        const bool bNewSchema = document.contains("Schema") ||
            document.contains("SchemaVersion") || document.contains("Rendering");
        if (bNewSchema)
        {
            if (!document.contains("Schema") || !document["Schema"].is_string() ||
                document["Schema"].get_ref<const std::string&>() !=
                    RenderSettingsSchema)
            {
                result.bSuccess = false;
                AddDiagnostic(result.Diagnostics, "Schema",
                    document.value("Schema", Json{}),
                    std::string(RenderSettingsSchema),
                    RenderSettingDiagnosticSeverity::Error, "unknown_schema",
                    "The render settings schema identifier is missing or unknown.");
                return result;
            }
            if (!document.contains("SchemaVersion") ||
                !document["SchemaVersion"].is_number_integer() ||
                document["SchemaVersion"].get<std::int64_t>() < 0)
            {
                result.bSuccess = false;
                AddDiagnostic(result.Diagnostics, "SchemaVersion",
                    document.value("SchemaVersion", Json{}),
                    RenderSettingsSchemaVersion,
                    RenderSettingDiagnosticSeverity::Error,
                    "invalid_schema_version",
                    "SchemaVersion must be an unsigned integer.");
                return result;
            }
            const std::uint64_t sourceVersion =
                document["SchemaVersion"].get<std::uint64_t>();
            if (sourceVersion > std::numeric_limits<std::uint32_t>::max())
            {
                result.bSuccess = false;
                result.bFutureSchema = true;
                AddDiagnostic(result.Diagnostics, "SchemaVersion",
                    sourceVersion, RenderSettingsSchemaVersion,
                    RenderSettingDiagnosticSeverity::Error,
                    "future_schema",
                    "SchemaVersion exceeds the supported range.");
                return result;
            }
            result.SourceVersion = static_cast<std::uint32_t>(sourceVersion);
            if (result.SourceVersion > RenderSettingsSchemaVersion)
            {
                result.bSuccess = false;
                result.bFutureSchema = true;
                AddDiagnostic(result.Diagnostics, "SchemaVersion",
                    result.SourceVersion, RenderSettingsSchemaVersion,
                    RenderSettingDiagnosticSeverity::Error, "future_schema",
                    "A newer render settings schema cannot be modified by this engine version.");
                return result;
            }
            result.bMigrated = result.SourceVersion < RenderSettingsSchemaVersion;
            for (const RenderSettingDescriptor& descriptor :
                GetRenderSettingDescriptors())
            {
                if (const Json* value = FindPath(document, descriptor.PropertyId))
                {
                    if (descriptor.bPersistedRenderSetting)
                        result.RenderValues[descriptor.PropertyId] = *value;
                    else if (descriptor.PropertyId ==
                        RenderSettingIds::CaptureSupersampleScale)
                        result.CaptureValues[descriptor.PropertyId] = *value;
                    else
                        result.DebugValues[descriptor.PropertyId] = *value;
                }
            }
            return result;
        }

        result.bMigrated = true;
        result.SourceVersion = 0;
        if (const Json* legacyGiSchema = FindPath(
                document, "Renderer.GlobalIllumination.Schema"))
        {
            if (!legacyGiSchema->is_string())
            {
                result.bSuccess = false;
                AddDiagnostic(result.Diagnostics,
                    "Renderer.GlobalIllumination.Schema", *legacyGiSchema,
                    "hengine.global-illumination-settings.v1",
                    RenderSettingDiagnosticSeverity::Error,
                    "invalid_legacy_schema",
                    "Legacy GI Schema must be a string.");
                return result;
            }
            if (legacyGiSchema->get_ref<const std::string&>() !=
                "hengine.global-illumination-settings.v1")
            {
                result.bSuccess = false;
                result.bFutureSchema = true;
                AddDiagnostic(result.Diagnostics,
                    "Renderer.GlobalIllumination.Schema", *legacyGiSchema,
                    "hengine.global-illumination-settings.v1",
                    RenderSettingDiagnosticSeverity::Error,
                    "future_legacy_schema",
                    "This legacy GI schema is unknown and cannot be overwritten.");
                return result;
            }
        }
        CopyIfPresent(result.RenderValues, RenderSettingIds::Backend,
            document, "Renderer.API");
        CopyIfPresent(result.RenderValues, RenderSettingIds::VSync,
            document, "Renderer.VSync");
        const std::array aaFields = {
            std::pair{ RenderSettingIds::AntiAliasingMode, "Mode"sv },
            std::pair{ RenderSettingIds::MsaaSamples, "MsaaSamples"sv },
            std::pair{ RenderSettingIds::FxaaEdgeThreshold, "FxaaEdgeThreshold"sv },
            std::pair{ RenderSettingIds::FxaaEdgeThresholdMin, "FxaaEdgeThresholdMin"sv },
            std::pair{ RenderSettingIds::FxaaSubpixelQuality, "FxaaSubpixelQuality"sv },
            std::pair{ RenderSettingIds::SmaaEdgeThreshold, "SmaaEdgeThreshold"sv },
            std::pair{ RenderSettingIds::SmaaMaxSearchSteps, "SmaaMaxSearchSteps"sv },
            std::pair{ RenderSettingIds::SmaaMaxDiagonalSearchSteps, "SmaaMaxDiagonalSearchSteps"sv },
            std::pair{ RenderSettingIds::SmaaCornerRounding, "SmaaCornerRounding"sv },
            std::pair{ RenderSettingIds::TaaHistoryWeight, "TaaHistoryWeight"sv },
            std::pair{ RenderSettingIds::TaaSharpness, "TaaSharpness"sv },
        };
        for (const auto& [id, name] : aaFields)
            CopyIfPresent(result.RenderValues, id, document,
                "Renderer.AntiAliasing." + std::string(name));
        CopyIfPresent(result.CaptureValues,
            RenderSettingIds::CaptureSupersampleScale, document,
            "Renderer.AntiAliasing.CaptureSupersampleScale");

        const std::array giFields = {
            std::pair{ RenderSettingIds::GiTechnique, "Technique"sv },
            std::pair{ RenderSettingIds::GiDenoiser, "Denoiser"sv },
            std::pair{ RenderSettingIds::GiUnsupportedFallback, "UnsupportedFallback"sv },
            std::pair{ RenderSettingIds::GiRenderScale, "RenderScale"sv },
            std::pair{ RenderSettingIds::GiSamplesPerPixel, "SamplesPerPixel"sv },
            std::pair{ RenderSettingIds::GiMaxBounces, "MaxBounces"sv },
            std::pair{ RenderSettingIds::GiAtrousIterations, "AtrousIterations"sv },
            std::pair{ RenderSettingIds::GiBilateralUpsample, "BilateralUpsample"sv },
        };
        for (const auto& [id, name] : giFields)
            CopyIfPresent(result.RenderValues, id, document,
                "Renderer.GlobalIllumination." + std::string(name));
        const std::array ssgiFields = {
            std::pair{ RenderSettingIds::GiSsgiRayCount, "RayCount"sv },
            std::pair{ RenderSettingIds::GiSsgiStepCount, "StepCount"sv },
            std::pair{ RenderSettingIds::GiSsgiHiZStartMip, "HiZStartMip"sv },
            std::pair{ RenderSettingIds::GiSsgiBinaryRefinementSteps, "BinaryRefinementSteps"sv },
            std::pair{ RenderSettingIds::GiSsgiMaximumRayDistance, "MaximumRayDistance"sv },
            std::pair{ RenderSettingIds::GiSsgiThickness, "Thickness"sv },
            std::pair{ RenderSettingIds::GiSsgiEdgeFade, "EdgeFade"sv },
            std::pair{ RenderSettingIds::GiSsgiTemporalWeight, "TemporalWeight"sv },
        };
        for (const auto& [id, name] : ssgiFields)
            CopyIfPresent(result.RenderValues, id, document,
                "Renderer.GlobalIllumination.SSGI." + std::string(name));
        CopyIfPresent(result.DebugValues, RenderSettingIds::GiDebugView,
            document, "Renderer.GlobalIllumination.DebugView");
        CopyIfPresent(result.DebugValues, RenderSettingIds::GiDirectOnly,
            document, "Renderer.GlobalIllumination.DirectOnly");
        AddDiagnostic(result.Diagnostics, "SchemaVersion", 0,
            RenderSettingsSchemaVersion,
            RenderSettingDiagnosticSeverity::Info, "legacy_migrated",
            "Legacy BaseEngine render settings were migrated in memory.");
        return result;
    }

    Json SerializeRenderSettingsLayerDocument(
        const Json& renderValues,
        const Json& debugValues,
        const Json& captureValues)
    {
        Json result{
            { "Schema", std::string(RenderSettingsSchema) },
            { "SchemaVersion", RenderSettingsSchemaVersion },
        };
        const auto append = [&result](const Json& values)
        {
            if (!values.is_object())
                return;
            for (const auto& [id, value] : values.items())
                SetPath(result, id, value);
        };
        append(renderValues);
        append(debugValues);
        append(captureValues);
        return result;
    }

    ResolvedRenderSettings ResolveRenderSettings(
        const RenderSettings& requested,
        const RenderDebugSettings& debug,
        const RenderCaptureSettings& capture,
        const RenderSettingsCapabilities& capabilities)
    {
        ResolvedRenderSettings result;
        result.Requested = requested;
        result.Effective = requested;
        result.Debug = debug;
        result.Capture = capture;
        result.Capabilities = capabilities;

        const RenderSettingAvailability backendAvailability =
            GetRenderBackendAvailability(requested.Backend, capabilities);
        const bool bBackendAvailable = backendAvailability.bAvailable;
        result.Availability[std::string(RenderSettingIds::Backend)] = {
            bBackendAvailable,
            backendAvailability.Reason };
        if (!bBackendAvailable)
        {
            result.Effective.Backend = RenderBackend::DX12;
            AddDiagnostic(result.Diagnostics, RenderSettingIds::Backend,
                ToString(requested.Backend), "DX12",
                RenderSettingDiagnosticSeverity::Warning,
                "capability_fallback",
                backendAvailability.Reason +
                    " DX12 is used without changing the request.");
        }

        if (HasAaCapabilities(capabilities.AntiAliasing))
        {
            const AntiAliasingSelection aa = ResolveAntiAliasingSelection(
                requested.AntiAliasing, capabilities.AntiAliasing,
                capabilities.AntiAliasingPath);
            result.Effective.AntiAliasing = aa.Effective;
            result.Availability[std::string(RenderSettingIds::AntiAliasingMode)] = {
                aa.bRequestedModeAvailable, aa.Reason };
            if (aa.bFallback || aa.bBypassed)
            {
                AddDiagnostic(result.Diagnostics,
                    RenderSettingIds::AntiAliasingMode,
                    ToString(aa.Requested.Mode), ToString(aa.Effective.Mode),
                    RenderSettingDiagnosticSeverity::Warning,
                    "capability_fallback", aa.Reason);
            }
        }
        GlobalIlluminationSelection gi;
        if (requested.GlobalIllumination.Technique ==
                GlobalIlluminationTechniqueKind::ExternalViewportRenderer &&
            capabilities.bExternalViewportRendererActive)
        {
            gi.Requested =
                GlobalIlluminationTechniqueKind::ExternalViewportRenderer;
            gi.Selected =
                GlobalIlluminationTechniqueKind::ExternalViewportRenderer;
            gi.Reason = "The requested offline viewport renderer is active.";
        }
        else
        {
            gi = ResolveGlobalIlluminationSelection(
                requested.GlobalIllumination, capabilities.Render,
                capabilities.bScreenSpaceGiAvailable);
        }
        result.Effective.GlobalIllumination.Technique = gi.Selected;
        result.Availability[std::string(RenderSettingIds::GiTechnique)] = {
            gi.bSupported, gi.Reason };
        if (gi.bFallback)
        {
            AddDiagnostic(result.Diagnostics, RenderSettingIds::GiTechnique,
                GiTechniqueName(gi.Requested), GiTechniqueName(gi.Selected),
                RenderSettingDiagnosticSeverity::Warning,
                "capability_fallback", gi.Reason);
        }
        if (requested.GlobalIllumination.Denoiser ==
                GlobalIlluminationDenoiserKind::Nrd)
        {
            result.Effective.GlobalIllumination.Denoiser =
                GlobalIlluminationDenoiserKind::Svgf;
            result.Availability[std::string(RenderSettingIds::GiDenoiser)] = {
                false, "NRD is not installed; SVGF is used." };
            AddDiagnostic(result.Diagnostics, RenderSettingIds::GiDenoiser,
                "NRD", "SVGF", RenderSettingDiagnosticSeverity::Warning,
                "plugin_fallback",
                "NRD is not installed; SVGF is used without changing the request.");
        }
        else
        {
            result.Availability[std::string(RenderSettingIds::GiDenoiser)] = {
                true, "The requested denoiser is available." };
        }
        return result;
    }

    RenderSettingAvailability GetRenderBackendAvailability(
        RenderBackend backend,
        const RenderSettingsCapabilities& capabilities)
    {
        const auto override = capabilities.BackendAvailability.find(backend);
        if (override != capabilities.BackendAvailability.end())
            return override->second;
        return {
            IsRenderBackendCompiled(backend),
            GetRenderBackendAvailabilityReason(backend),
        };
    }

    RenderSettingsChangePlan BuildRenderSettingsChangePlan(
        const ResolvedRenderSettings& before,
        const ResolvedRenderSettings& after)
    {
        RenderSettingsChangePlan result;
        Json beforeValues = RenderSettingsToFlatValues(before.Requested);
        Json afterValues = RenderSettingsToFlatValues(after.Requested);
        beforeValues.update(RenderDebugSettingsToFlatValues(before.Debug));
        beforeValues.update(RenderCaptureSettingsToFlatValues(before.Capture));
        afterValues.update(RenderDebugSettingsToFlatValues(after.Debug));
        afterValues.update(RenderCaptureSettingsToFlatValues(after.Capture));
        std::set<ApplyImpact> impacts;
        for (const auto& [id, value] : afterValues.items())
        {
            if (beforeValues.contains(id) && beforeValues[id] == value)
                continue;
            result.ChangedPropertyIds.push_back(id);
            if (const RenderSettingDescriptor* descriptor =
                    FindRenderSettingDescriptor(id))
                impacts.insert(descriptor->Impact);
        }
        result.Impacts.assign(impacts.begin(), impacts.end());
        for (ApplyImpact impact : impacts)
        {
            result.HighestImpact = std::max(result.HighestImpact, impact);
            switch (impact)
            {
            case ApplyImpact::Live:
                break;
            case ApplyImpact::TemporalHistoryReset:
                result.bTemporalHistoryResetRequired = true;
                break;
            case ApplyImpact::RenderGraphRebuild:
                result.bRenderGraphRebuildRequired = true;
                result.bTemporalHistoryResetRequired = true;
                break;
            case ApplyImpact::RenderTargetRecreate:
                result.bRenderTargetRecreateRequired = true;
                result.bRenderGraphRebuildRequired = true;
                result.bTemporalHistoryResetRequired = true;
                break;
            case ApplyImpact::SwapchainRecreate:
                result.bSwapchainRecreateRequired = true;
                break;
            case ApplyImpact::ProcessRestart:
                result.bProcessRestartRequired = true;
                break;
            case ApplyImpact::BuildReconfigure:
                result.bBuildReconfigureRequired = true;
                result.bProcessRestartRequired = true;
                break;
            }
        }
        result.bGpuIdleRequired = result.bRenderGraphRebuildRequired ||
            result.bRenderTargetRecreateRequired ||
            result.bSwapchainRecreateRequired;
        return result;
    }

    Json GetGlobalIlluminationQualityPresetPatch(Preset preset)
    {
        switch (preset)
        {
        case Preset::Low:
            return Json{
                { RenderSettingIds::GiRenderScale, 0.5 },
                { RenderSettingIds::GiMaxBounces, 2 },
                { RenderSettingIds::GiAtrousIterations, 2 },
                { RenderSettingIds::GiSsgiRayCount, 1 },
                { RenderSettingIds::GiSsgiStepCount, 24 },
            };
        case Preset::Balanced:
            return Json{
                { RenderSettingIds::GiRenderScale, 0.5 },
                { RenderSettingIds::GiMaxBounces, 4 },
                { RenderSettingIds::GiAtrousIterations, 3 },
                { RenderSettingIds::GiSsgiRayCount, 2 },
                { RenderSettingIds::GiSsgiStepCount, 64 },
            };
        case Preset::High:
            return Json{
                { RenderSettingIds::GiRenderScale, 1.0 },
                { RenderSettingIds::GiMaxBounces, 4 },
                { RenderSettingIds::GiAtrousIterations, 4 },
                { RenderSettingIds::GiSsgiRayCount, 8 },
                { RenderSettingIds::GiSsgiStepCount, 96 },
            };
        case Preset::Custom:
            return Json::object();
        }
        return Json::object();
    }

    GlobalIlluminationQualityPreset DetectGlobalIlluminationQualityPreset(
        const RenderSettings& settings)
    {
        const Json values = RenderSettingsToFlatValues(settings);
        for (Preset preset : { Preset::Low, Preset::Balanced, Preset::High })
        {
            const Json patch = GetGlobalIlluminationQualityPresetPatch(preset);
            bool bMatches = true;
            for (const auto& [id, value] : patch.items())
                bMatches &= values.contains(id) && values[id] == value;
            if (bMatches)
                return preset;
        }
        return Preset::Custom;
    }

    std::string HashResolvedRenderSettings(const ResolvedRenderSettings& settings)
    {
        Json values = RenderSettingsToFlatValues(settings.Requested);
        values.update(RenderDebugSettingsToFlatValues(settings.Debug));
        values.update(RenderCaptureSettingsToFlatValues(settings.Capture));
        const std::string canonical = values.dump();
        std::uint64_t hash = 1469598103934665603ull;
        for (const unsigned char character : canonical)
        {
            hash ^= character;
            hash *= 1099511628211ull;
        }
        std::ostringstream stream;
        stream << std::hex << std::setfill('0') << std::setw(16) << hash;
        return stream.str();
    }

    bool HasDiagnosticErrors(
        const std::vector<RenderSettingDiagnostic>& diagnostics) noexcept
    {
        return std::ranges::any_of(diagnostics,
            [](const RenderSettingDiagnostic& diagnostic)
            {
                return diagnostic.Severity ==
                    RenderSettingDiagnosticSeverity::Error;
            });
    }
}
