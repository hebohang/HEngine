#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationSettingsJson.h"

#include <algorithm>
#include <string>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] const char* TechniqueName(
            GlobalIlluminationTechniqueKind technique) noexcept
        {
            switch (technique)
            {
            case GlobalIlluminationTechniqueKind::Off: return "Off";
            case GlobalIlluminationTechniqueKind::ScreenSpace: return "ScreenSpaceGI";
            case GlobalIlluminationTechniqueKind::HardwareRayTracing: return "HardwareRTGI";
            case GlobalIlluminationTechniqueKind::ExternalViewportRenderer:
                return "ExternalViewportRenderer";
            }
            return "Off";
        }

        [[nodiscard]] GlobalIlluminationTechniqueKind ParseTechnique(
            const std::string& value,
            GlobalIlluminationTechniqueKind fallback) noexcept
        {
            if (value == "Off") return GlobalIlluminationTechniqueKind::Off;
            if (value == "ScreenSpaceGI")
                return GlobalIlluminationTechniqueKind::ScreenSpace;
            if (value == "HardwareRTGI")
                return GlobalIlluminationTechniqueKind::HardwareRayTracing;
            if (value == "ExternalViewportRenderer")
                return GlobalIlluminationTechniqueKind::ExternalViewportRenderer;
            return fallback;
        }

        [[nodiscard]] const char* DenoiserName(
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

        [[nodiscard]] GlobalIlluminationDenoiserKind ParseDenoiser(
            const std::string& value,
            GlobalIlluminationDenoiserKind fallback) noexcept
        {
            if (value == "None") return GlobalIlluminationDenoiserKind::None;
            if (value == "SVGF") return GlobalIlluminationDenoiserKind::Svgf;
            if (value == "NRD") return GlobalIlluminationDenoiserKind::Nrd;
            return fallback;
        }

        [[nodiscard]] const char* FallbackName(
            GlobalIlluminationFallbackKind fallback) noexcept
        {
            return fallback == GlobalIlluminationFallbackKind::ScreenSpace
                ? "ScreenSpaceGI"
                : "Off";
        }

        [[nodiscard]] GlobalIlluminationFallbackKind ParseFallback(
            const std::string& value,
            GlobalIlluminationFallbackKind fallback) noexcept
        {
            if (value == "Off") return GlobalIlluminationFallbackKind::Off;
            if (value == "ScreenSpaceGI")
                return GlobalIlluminationFallbackKind::ScreenSpace;
            return fallback;
        }
    }

    Json SerializeGlobalIlluminationSettings(
        const GlobalIlluminationSettings& settings)
    {
        return Json{
            { "Schema", "hengine.global-illumination-settings.v1" },
            { "Technique", TechniqueName(settings.Technique) },
            { "Denoiser", DenoiserName(settings.Denoiser) },
            { "UnsupportedFallback", FallbackName(settings.UnsupportedFallback) },
            { "RenderScale", settings.RenderScale },
            { "SamplesPerPixel", settings.SamplesPerPixel },
            { "MaxBounces", settings.MaxBounces },
            { "AtrousIterations", settings.AtrousIterations },
            { "SvgfOptimizedKernels", settings.bSvgfOptimizedKernels },
            { "SsgiLocalReactiveBudget", settings.bSsgiLocalReactiveBudget },
            { "SvgfSharedUncertainty", settings.bSvgfSharedUncertainty },
            { "SsgiEnvironmentMis", settings.bSsgiEnvironmentMis },
            { "BilateralUpsample", settings.bBilateralUpsample },
            { "SSGI", {
                { "RayCount", settings.ScreenSpaceRayCount },
                { "StepCount", settings.ScreenSpaceStepCount },
                { "HiZStartMip", settings.ScreenSpaceHiZStartMip },
                { "BinaryRefinementSteps",
                    settings.ScreenSpaceBinaryRefinementSteps },
                { "MaximumRayDistance",
                    settings.ScreenSpaceMaximumRayDistance },
                { "Thickness", settings.ScreenSpaceThickness },
                { "EdgeFade", settings.ScreenSpaceEdgeFade },
                { "TemporalWeight", settings.ScreenSpaceTemporalWeight },
                { "DiffuseHitDistanceWeight", settings.ScreenSpaceDiffuseHitDistanceWeight },
            } },
        };
    }

    GlobalIlluminationSettings DeserializeGlobalIlluminationSettings(
        const Json& json,
        const GlobalIlluminationSettings& defaults)
    {
        if (!json.is_object())
            return defaults;
        GlobalIlluminationSettings result = defaults;
        result.Technique = ParseTechnique(
            json.value("Technique", std::string(TechniqueName(defaults.Technique))),
            defaults.Technique);
        result.Denoiser = ParseDenoiser(
            json.value(
                "Denoiser",
                std::string(DenoiserName(defaults.Denoiser))),
            defaults.Denoiser);
        result.UnsupportedFallback = ParseFallback(
            json.value(
                "UnsupportedFallback",
                std::string(FallbackName(defaults.UnsupportedFallback))),
            defaults.UnsupportedFallback);
        const uint32_t debugView = json.value(
            "DebugView",
            static_cast<uint32_t>(defaults.DebugView));
        if (debugView <= static_cast<uint32_t>(GlobalIlluminationDebugView::Variance))
            result.DebugView = static_cast<GlobalIlluminationDebugView>(debugView);
        result.RenderScale = std::clamp(
            json.value("RenderScale", defaults.RenderScale),
            0.5f,
            1.0f);
        result.SamplesPerPixel = std::clamp(
            json.value("SamplesPerPixel", defaults.SamplesPerPixel),
            1u,
            16u);
        result.MaxBounces = std::clamp(
            json.value("MaxBounces", defaults.MaxBounces),
            1u,
            8u);
        result.AtrousIterations = std::clamp(
            json.value("AtrousIterations", defaults.AtrousIterations),
            1u,
            5u);
        result.bBilateralUpsample = json.value(
            "BilateralUpsample",
            defaults.bBilateralUpsample);
        result.bSvgfOptimizedKernels = json.value("SvgfOptimizedKernels", defaults.bSvgfOptimizedKernels);
        result.bSsgiLocalReactiveBudget = json.value("SsgiLocalReactiveBudget", defaults.bSsgiLocalReactiveBudget);
        result.bSvgfSharedUncertainty = json.value("SvgfSharedUncertainty", defaults.bSvgfSharedUncertainty);
        result.bSsgiEnvironmentMis = json.value("SsgiEnvironmentMis", defaults.bSsgiEnvironmentMis);
        result.bDirectOnly = json.value("DirectOnly", defaults.bDirectOnly);
        if (json.contains("SSGI") && json["SSGI"].is_object())
        {
            const Json& ssgi = json["SSGI"];
            result.ScreenSpaceRayCount = std::clamp(
                ssgi.value("RayCount", defaults.ScreenSpaceRayCount), 1u, 8u);
            result.ScreenSpaceStepCount = std::clamp(
                ssgi.value("StepCount", defaults.ScreenSpaceStepCount), 1u, 256u);
            result.ScreenSpaceHiZStartMip = std::min(
                ssgi.value("HiZStartMip", defaults.ScreenSpaceHiZStartMip), 16u);
            result.ScreenSpaceBinaryRefinementSteps = std::min(
                ssgi.value(
                    "BinaryRefinementSteps",
                    defaults.ScreenSpaceBinaryRefinementSteps),
                8u);
            result.ScreenSpaceMaximumRayDistance = std::max(
                ssgi.value(
                    "MaximumRayDistance",
                    defaults.ScreenSpaceMaximumRayDistance),
                0.001f);
            result.ScreenSpaceThickness = std::max(
                ssgi.value("Thickness", defaults.ScreenSpaceThickness),
                0.0001f);
            result.ScreenSpaceEdgeFade = std::clamp(
                ssgi.value("EdgeFade", defaults.ScreenSpaceEdgeFade),
                0.0f,
                0.5f);
            result.ScreenSpaceDiffuseHitDistanceWeight = std::clamp(
                ssgi.value("DiffuseHitDistanceWeight", defaults.ScreenSpaceDiffuseHitDistanceWeight),
                0.0f, 1.0f);
            result.ScreenSpaceTemporalWeight = std::clamp(
                ssgi.value(
                    "TemporalWeight",
                    defaults.ScreenSpaceTemporalWeight),
                0.0f,
                0.999f);
        }
        return result;
    }
}
