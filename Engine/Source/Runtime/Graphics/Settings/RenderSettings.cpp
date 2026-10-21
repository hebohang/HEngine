#include "Hepch.h"

#include "Runtime/Graphics/Settings/RenderSettings.h"
#include "Runtime/Graphics/Renderer/RenderFeature.h"
#include "Runtime/Graphics/Renderer/RenderPipelineRegistry.h"
#include "Runtime/Graphics/Lighting/PhotometricUnits.h"

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
            result.reserve(96);
            result.push_back(Descriptor(RenderSettingIds::ViewOverrides, RenderSettingValueType::Json,
                defaults.ViewOverrides, "Rendering", "Views", "Per-view overrides", ApplyImpact::RenderGraphRebuild));
            result.back().WritableLayers = { SettingsLayer::Project, SettingsLayer::User, SettingsLayer::Session };
            result.push_back(Descriptor(RenderSettingIds::PipelineType, RenderSettingValueType::String,
                defaults.PipelineType, "Rendering", "Pipeline", "Pipeline implementation", ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::PipelineProfile, RenderSettingValueType::String,
                defaults.PipelineProfile, "Rendering", "Pipeline", "Pipeline profile", ApplyImpact::Live));
            result.push_back(Descriptor(RenderSettingIds::PipelineFeatures, RenderSettingValueType::Json,
                defaults.PipelineFeatures, "Rendering", "Pipeline", "Rendering features", ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::CloudQuality, RenderSettingValueType::Enumeration, "Off", "Rendering", "Clouds", "Cloud Quality", ApplyImpact::RenderGraphRebuild, {}, {}, false, { "Off", "Low", "Balanced", "High", "Custom" }));
            result.push_back(Descriptor(RenderSettingIds::CloudRenderScale, RenderSettingValueType::Float, 0.5, "Rendering", "Clouds", "Cloud RenderScale", ApplyImpact::RenderTargetRecreate, 0.25, 1));
            result.back().Unit = "ratio-per-axis";
            result.push_back(Descriptor(RenderSettingIds::CloudMaxSteps, RenderSettingValueType::Integer, 40, "Rendering", "Clouds", "Cloud MaxSteps", ApplyImpact::TemporalHistoryReset, 16, 96));
            result.push_back(Descriptor(RenderSettingIds::CloudLightSteps, RenderSettingValueType::Integer, 3, "Rendering", "Clouds", "Cloud LightSteps", ApplyImpact::TemporalHistoryReset, 0, 6));
            result.push_back(Descriptor(RenderSettingIds::CloudHistoryWeight, RenderSettingValueType::Float, 0.85, "Rendering", "Clouds", "Cloud HistoryWeight", ApplyImpact::TemporalHistoryReset, 0, 0.95));
            result.push_back(Descriptor(RenderSettingIds::CloudDetailDistance, RenderSettingValueType::Float, 1800.0, "Rendering", "Clouds", "Cloud DetailDistance", ApplyImpact::TemporalHistoryReset, 100, 20000));
            result.back().Unit = "m";
            result.push_back(Descriptor(RenderSettingIds::CloudTemporal, RenderSettingValueType::Boolean, true, "Rendering", "Clouds", "Cloud Temporal", ApplyImpact::TemporalHistoryReset));
            result.push_back(Descriptor(RenderSettingIds::CloudDebugView, RenderSettingValueType::Integer, 0, "Rendering", "Clouds", "Cloud DebugView", ApplyImpact::TemporalHistoryReset, 0, 3));

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
            result.back().WritableLayers = { SettingsLayer::Project, SettingsLayer::User, SettingsLayer::Session };
            result.push_back(Descriptor(RenderSettingIds::FramesInFlight,
                RenderSettingValueType::Integer, defaults.FramesInFlight,
                "Rendering", "Presentation", "Frames in flight",
                ApplyImpact::ProcessRestart, 2, 3));
            result.back().bRestartRequired = true;
            result.back().WritableLayers = { SettingsLayer::Project, SettingsLayer::User };
            result.push_back(Descriptor(RenderSettingIds::Path,
                RenderSettingValueType::Enumeration, "Deferred",
                "Rendering", "Pipeline", "Rendering path",
                ApplyImpact::RenderTargetRecreate, {}, {}, false,
                { "Forward", "Deferred" }));
            result.push_back(Descriptor(RenderSettingIds::GpuOcclusionEnabled,
                RenderSettingValueType::Boolean, false, "Rendering", "Visibility", "Same-frame GPU occlusion",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::GpuTightFrustumEnabled,
                RenderSettingValueType::Boolean, false, "Rendering", "Visibility", "Tight main-view AABB broad phase",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::GpuBatchingEnabled,
                RenderSettingValueType::Boolean, false, "Rendering", "Visibility", "GPU compacted compatible draw groups",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::TransientCacheBudgetMiB,
                RenderSettingValueType::Integer, 0, "Rendering", "Memory", "Transient resource cache budget",
                ApplyImpact::Live, 0, 16384));
            result.back().Unit = "MiB";
            result.back().WritableLayers = { SettingsLayer::Project, SettingsLayer::User, SettingsLayer::Session };
            result.push_back(Descriptor(RenderSettingIds::TransientTextureReuse,
                RenderSettingValueType::Boolean, false, "Rendering", "Memory", "Reuse disjoint transient textures",
                ApplyImpact::RenderTargetRecreate));
            result.push_back(Descriptor(RenderSettingIds::GpuShadowBatchingEnabled,
                RenderSettingValueType::Boolean, false, "Rendering", "Visibility", "GPU compacted shadow draw groups",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::LodEnabled,
                RenderSettingValueType::Boolean, false, "Rendering", "Visibility", "Resource mesh LOD",
                ApplyImpact::TemporalHistoryReset));
            result.push_back(Descriptor(RenderSettingIds::LodMaxPixelError,
                RenderSettingValueType::Float, 1.0, "Rendering", "Visibility", "LOD maximum projected error in pixels",
                ApplyImpact::TemporalHistoryReset, 0.1, 8.0));
            result.push_back(Descriptor(RenderSettingIds::AoEnabled,
                RenderSettingValueType::Boolean, true, "Rendering", "AmbientOcclusion", "Ambient occlusion",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::AoTechnique,
                RenderSettingValueType::Enumeration, "GTAO", "Rendering", "AmbientOcclusion", "AO technique",
                ApplyImpact::RenderGraphRebuild, {}, {}, false, { "GTAO", "SSAO" }));
            result.push_back(Descriptor(RenderSettingIds::AoNormalBias,
                RenderSettingValueType::Float, 0.01, "Rendering", "AmbientOcclusion", "AO world-space normal bias",
                ApplyImpact::Live, 0.0, 0.25));
            result.push_back(Descriptor(RenderSettingIds::AoSampleCount,
                RenderSettingValueType::Integer, 16, "Rendering", "AmbientOcclusion", "SSAO samples",
                ApplyImpact::Live, 8, 64));
            result.push_back(Descriptor(RenderSettingIds::AoFastUpsample,
                RenderSettingValueType::Boolean, false, "Rendering", "AmbientOcclusion", "AO four-tap bilateral upsample",
                ApplyImpact::Live));
            result.push_back(Descriptor(RenderSettingIds::AoRadius,
                RenderSettingValueType::Float, 1.0, "Rendering", "AmbientOcclusion", "AO world-space radius",
                ApplyImpact::Live, 0.01, 10.0));
            result.push_back(Descriptor(RenderSettingIds::AoIntensity,
                RenderSettingValueType::Float, 1.0, "Rendering", "AmbientOcclusion", "AO intensity",
                ApplyImpact::Live, 0.0, 4.0));
            result.push_back(Descriptor(RenderSettingIds::SsrEnabled,
                RenderSettingValueType::Boolean, true, "Rendering", "Reflections", "Screen-space reflections",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::AoSliceCount,
                RenderSettingValueType::Integer, 4, "Rendering", "AmbientOcclusion", "GTAO horizon slices",
                ApplyImpact::Live, 2, 8));
            result.push_back(Descriptor(RenderSettingIds::AoStepsPerSide,
                RenderSettingValueType::Integer, 8, "Rendering", "AmbientOcclusion", "GTAO steps on each side",
                ApplyImpact::Live, 2, 16));
            result.push_back(Descriptor(RenderSettingIds::SsrGuideCache,
                RenderSettingValueType::Boolean, false, "Rendering", "Reflections", "SSR shared guide cache",
                ApplyImpact::Live));
            result.push_back(Descriptor(RenderSettingIds::ReflectionProbesEnabled,
                RenderSettingValueType::Boolean, true, "Rendering", "Reflections", "Local reflection probes",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::SsrMaximumDistance,
                RenderSettingValueType::Float, 50.0, "Rendering", "Reflections", "SSR world-space maximum distance",
                ApplyImpact::Live, 0.1, 500.0));
            result.push_back(Descriptor(RenderSettingIds::SsrThickness,
                RenderSettingValueType::Float, 0.2, "Rendering", "Reflections", "SSR world-space thickness",
                ApplyImpact::Live, 0.001, 10.0));
            result.push_back(Descriptor(RenderSettingIds::SsrRoughnessCutoff,
                RenderSettingValueType::Float, 0.6, "Rendering", "Reflections", "SSR roughness cutoff",
                ApplyImpact::Live, 0.05, 1.0));
            result.push_back(Descriptor(RenderSettingIds::SsrEdgeFade,
                RenderSettingValueType::Float, 0.08, "Rendering", "Reflections", "SSR screen-edge fade fraction",
                ApplyImpact::Live, 0.001, 0.5));
            result.push_back(Descriptor(RenderSettingIds::SsrNormalBias,
                RenderSettingValueType::Float, 0.03, "Rendering", "Reflections", "SSR world-space normal bias",
                ApplyImpact::Live, 0.0, 1.0));
            result.push_back(Descriptor(RenderSettingIds::SsrStepCount,
                RenderSettingValueType::Integer, 64, "Rendering", "Reflections", "SSR maximum march steps",
                ApplyImpact::Live, 8, 256));
            result.push_back(Descriptor(RenderSettingIds::SsrBinaryRefinementSteps,
                RenderSettingValueType::Integer, 5, "Rendering", "Reflections", "SSR hit refinement steps",
                ApplyImpact::Live, 0, 12));
            result.push_back(Descriptor(RenderSettingIds::AntiAliasingMode,
                RenderSettingValueType::Enumeration,
                ToString(defaults.AntiAliasing.Mode), "Rendering",
                "AntiAliasing", "Anti-aliasing mode",
                ApplyImpact::RenderTargetRecreate, {}, {}, false,
                { "None", "FXAA", "SMAA", "TAA", "MSAA" },
                "Rendering.AntiAliasing.Mode.Available"));
            result.push_back(Descriptor(RenderSettingIds::RenderHeight,
                RenderSettingValueType::Integer, defaults.PostProcess.RenderHeight,
                "Rendering", "Resolution", "3D render height cap (0 = native)",
                ApplyImpact::RenderTargetRecreate, 0, 16384));
            result.push_back(Descriptor(RenderSettingIds::TextureStreamingBudget,
                RenderSettingValueType::Integer, defaults.TextureStreamingBudgetMiB,
                "Rendering", "TextureStreaming", "Material texture budget", ApplyImpact::Live, 1, 8192));
            result.back().Unit = "MiB";
            result.back().WritableLayers = { SettingsLayer::Project, SettingsLayer::User, SettingsLayer::Session };
            result.push_back(Descriptor(RenderSettingIds::UpscalingMode,
                RenderSettingValueType::Enumeration, ToString(defaults.Upscaling.Mode),
                "Rendering", "Upscaling", "Temporal upscaler",
                ApplyImpact::RenderTargetRecreate, {}, {}, false, {"Off", "FSR3.1"}));
            result.push_back(Descriptor(RenderSettingIds::UpscalingQuality,
                RenderSettingValueType::Enumeration, ToString(defaults.Upscaling.Quality),
                "Rendering", "Upscaling", "Upscaling quality",
                ApplyImpact::RenderTargetRecreate, {}, {}, false,
                {"NativeAA", "Quality", "Balanced", "Performance", "UltraPerformance"}));
            result.push_back(Descriptor(RenderSettingIds::UpscalingSharpness,
                RenderSettingValueType::Float, defaults.Upscaling.Sharpness,
                "Rendering", "Upscaling", "FSR sharpness", ApplyImpact::Live, 0.0, 1.0, true));
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
            result.push_back(Descriptor(RenderSettingIds::TaaOptimizedHistoryGather,
                RenderSettingValueType::Boolean, false, "Rendering", "AntiAliasing",
                "TAA exact guide-first history gather", ApplyImpact::TemporalHistoryReset));
            result.push_back(Descriptor(RenderSettingIds::TaaSharedNeighborhood,
                RenderSettingValueType::Boolean, false, "Rendering", "AntiAliasing",
                "TAA shared current neighborhood", ApplyImpact::TemporalHistoryReset));

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
            result.push_back(Descriptor(RenderSettingIds::GiSsgiEnvironmentMis,
                RenderSettingValueType::Boolean, defaults.GlobalIllumination.bSsgiEnvironmentMis, "Rendering",
                "GlobalIllumination.ScreenSpace", "Environment importance sampling",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::GiSvgfOptimizedKernels,
                RenderSettingValueType::Boolean, false, "Rendering",
                "GlobalIllumination.Denoising", "SVGF equivalent kernel optimizations",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::GiSsgiLocalReactiveBudget,
                RenderSettingValueType::Boolean, false, "Rendering",
                "GlobalIllumination.Denoising", "Local SSGI reactive budget (candidate)",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::GiSvgfSharedUncertainty,
                RenderSettingValueType::Boolean, false, "Rendering",
                "GlobalIllumination.Denoising", "Share SVGF uncertainty prefilter",
                ApplyImpact::RenderGraphRebuild));
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
                "SSGI binary refinement steps (legacy; unused by analytic tracing)",
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
            result.push_back(Descriptor(RenderSettingIds::GiSsgiDiffuseHitDistanceWeight,
                RenderSettingValueType::Float,
                defaults.GlobalIllumination.ScreenSpaceDiffuseHitDistanceWeight,
                "Rendering", "GlobalIllumination.ScreenSpaceGI",
                "SSGI diffuse secondary distance guide weight", ApplyImpact::RenderGraphRebuild,
                0.0, 1.0, true));
            result.push_back(Descriptor(RenderSettingIds::GiSsgiTemporalWeight,
                RenderSettingValueType::Float,
                defaults.GlobalIllumination.ScreenSpaceTemporalWeight,
                "Rendering", "GlobalIllumination.ScreenSpaceGI",
                "SSGI temporal weight", ApplyImpact::TemporalHistoryReset,
                0.0, 0.999, true));
            result.push_back(Descriptor(RenderSettingIds::PostProcessExposure,
                RenderSettingValueType::Float, -std::log2(defaults.PostProcess.Exposure),
                "Rendering", "PostProcess", "Default exposure (EV100)",
                ApplyImpact::Live, -24.0, 32.0));
            result.push_back(Descriptor(RenderSettingIds::BloomIntensity,
                RenderSettingValueType::Float, defaults.PostProcess.BloomIntensity,
                "Rendering", "PostProcess.Bloom", "Bloom intensity",
                ApplyImpact::RenderGraphRebuild, 0.0, 2.0));
            result.push_back(Descriptor(RenderSettingIds::BloomThreshold,
                RenderSettingValueType::Float, defaults.PostProcess.BloomThreshold,
                "Rendering", "PostProcess.Bloom", "Bloom scene-linear threshold",
                ApplyImpact::Live, 0.0, 100.0));
            result.push_back(Descriptor(RenderSettingIds::AutoExposure,
                RenderSettingValueType::Boolean, defaults.PostProcess.bAutoExposure,
                "Rendering", "PostProcess.AutoExposure", "Automatic exposure",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::ExposureKey,
                RenderSettingValueType::Float, defaults.PostProcess.ExposureKey,
                "Rendering", "PostProcess.AutoExposure", "Exposure middle gray",
                ApplyImpact::Live, 0.01, 1.0));
            result.push_back(Descriptor(RenderSettingIds::MinimumExposure,
                RenderSettingValueType::Float, -std::log2(defaults.PostProcess.MaximumExposure),
                "Rendering", "PostProcess.AutoExposure", "Minimum EV100",
                ApplyImpact::Live, -24.0, 32.0));
            result.push_back(Descriptor(RenderSettingIds::MaximumExposure,
                RenderSettingValueType::Float, -std::log2(defaults.PostProcess.MinimumExposure),
                "Rendering", "PostProcess.AutoExposure", "Maximum EV100",
                ApplyImpact::Live, -24.0, 32.0));
            result.push_back(Descriptor(RenderSettingIds::ExposureAdaptationSpeed,
                RenderSettingValueType::Float, defaults.PostProcess.ExposureAdaptationSpeed,
                "Rendering", "PostProcess.AutoExposure", "Exposure adaptation rate per second",
                ApplyImpact::Live, 0.01, 20.0));
            result.push_back(Descriptor(RenderSettingIds::ColorContrast,
                RenderSettingValueType::Float, defaults.PostProcess.Contrast,
                "Rendering", "PostProcess.Color", "Color contrast",
                ApplyImpact::Live, 0.1, 3.0));
            result.push_back(Descriptor(RenderSettingIds::ColorSaturation,
                RenderSettingValueType::Float, defaults.PostProcess.Saturation,
                "Rendering", "PostProcess.Color", "Color saturation",
                ApplyImpact::Live, 0.0, 2.0));
            result.push_back(Descriptor(RenderSettingIds::ColorTemperature,
                RenderSettingValueType::Float, defaults.PostProcess.Temperature,
                "Rendering", "PostProcess.Color", "Warm/cool color balance",
                ApplyImpact::Live, -1.0, 1.0));
            result.push_back(Descriptor(RenderSettingIds::LocalExposureShadowStrength,
                RenderSettingValueType::Float, defaults.PostProcess.LocalExposureShadowStrength,
                "Rendering", "PostProcess.LocalExposure", "Local shadow lift",
                ApplyImpact::RenderGraphRebuild, 0.0, 1.0));
            result.push_back(Descriptor(RenderSettingIds::ExposureCompensationEV,
                RenderSettingValueType::Float, defaults.PostProcess.ExposureCompensationEV,
                "Rendering", "PostProcess", "Player exposure compensation (stops)",
                ApplyImpact::Live, -4.0, 4.0));
            result.push_back(Descriptor(RenderSettingIds::LocalExposureHighlightStrength,
                RenderSettingValueType::Float, defaults.PostProcess.LocalExposureHighlightStrength,
                "Rendering", "PostProcess.LocalExposure", "Local highlight compression",
                ApplyImpact::RenderGraphRebuild, 0.0, 1.0));
            result.push_back(Descriptor(RenderSettingIds::LocalExposureMaxEV,
                RenderSettingValueType::Float, defaults.PostProcess.LocalExposureMaxEV,
                "Rendering", "PostProcess.LocalExposure", "Maximum local exposure adjustment (stops)",
                ApplyImpact::RenderGraphRebuild, 0.0, 4.0));
            result.push_back(Descriptor(RenderSettingIds::SkinSubsurfaceEnabled,
                RenderSettingValueType::Boolean, defaults.PostProcess.SkinSubsurfaceEnabled,
                "Rendering", "SkinSubsurface", "Deferred skin diffusion",
                ApplyImpact::RenderGraphRebuild));
            result.push_back(Descriptor(RenderSettingIds::SkinSubsurfaceRadiusMeters,
                RenderSettingValueType::Float, defaults.PostProcess.SkinSubsurfaceRadiusMeters,
                "Rendering", "SkinSubsurface", "Skin diffusion radius in meters",
                ApplyImpact::Live, 0.0001, 0.02));
            result.push_back(Descriptor(RenderSettingIds::SkinSubsurfaceQuality,
                RenderSettingValueType::Integer, defaults.PostProcess.SkinSubsurfaceQuality,
                "Rendering", "SkinSubsurface", "Skin diffusion quality (1 = 7 taps, 2 = 13 taps)",
                ApplyImpact::Live, 1, 2));
            result.push_back(Descriptor(RenderSettingIds::ShadowsEnabled,
                RenderSettingValueType::Boolean, defaults.Shadows.bEnabled,
                "Rendering", "Shadows", "Enable dynamic shadows",
                ApplyImpact::RenderGraphRebuild));

            result.push_back(Descriptor(RenderSettingIds::ContinuousPcfEnabled,
                RenderSettingValueType::Boolean, defaults.Shadows.bContinuousPcfEnabled,
                "Rendering", "Shadows", "Continuous weighted comparison PCF",
                ApplyImpact::RenderGraphRebuild));

            auto debugView = Descriptor(RenderSettingIds::GiDebugView,
                RenderSettingValueType::Integer, 0, "Rendering",
                "GlobalIllumination.Diagnostics", "GI debug view",
                ApplyImpact::RenderGraphRebuild, 0.0, 20.0, true);
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
                "PBR debug mode (12 = screen-space AO, 13 = logarithmic view depth)", ApplyImpact::Live, 0.0, 13.0, true);
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
            case RenderSettingValueType::Json:
                if (descriptor.PropertyId == RenderSettingIds::ViewOverrides)
                {
                    if (!input.is_object()) return fail("Expected named view override objects.");
                    for (const auto& [view, patch] : input.items())
                    {
                        if (view != "main" && view != "editor" && view != "preview") return fail("Unknown view scope.");
                        if (!patch.is_object()) return fail("View overrides must be an object.");
                        for (const auto& [id, value] : patch.items())
                        {
                            const auto* field = FindRenderSettingDescriptor(id);
                            if (!field || !field->bPersistedRenderSetting ||
                                std::ranges::find(field->WritableLayers, SettingsLayer::Camera) == field->WritableLayers.end())
                                return fail("View override cannot set device, nested view, or tool settings: " + id);
                            if (value.is_null() || NormalizeValue(*field, value, diagnostics).is_null())
                                return fail("Invalid view override value: " + id);
                        }
                    }
                    return input;
                }
                if (!input.is_array()) return fail("Expected a Feature array.");
                {
                    std::set<std::string> instances;
                    for (const auto& feature : input)
                    {
                        if (!feature.is_object() || !feature.contains("type") || !feature["type"].is_string() ||
                            !feature.contains("instance") || !feature["instance"].is_string() ||
                            !feature.contains("version") || (!feature["version"].is_number_integer() || feature["version"].get<double>() < 1 || feature["version"].get<double>() > 4294967295.0) ||
                            !feature.contains("enabled") || !feature["enabled"].is_boolean() ||
                            !feature.contains("parameters") || !feature["parameters"].is_object())
                            return fail("Feature requires type, instance, version, enabled and parameters.");
                        const auto instance = feature["instance"].get<std::string>();
                        if (instance.empty() || feature["type"].get<std::string>().empty() ||
                            !instances.insert(instance).second)
                            return fail("Feature instance IDs must be nonempty and unique.");
                    }
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
        settings.GlobalIllumination.RenderScale = 0.5f;
        settings.GlobalIllumination.MaxBounces = 1;
        settings.GlobalIllumination.AtrousIterations = 2;
        settings.GlobalIllumination.bSvgfOptimizedKernels = true;
        settings.GlobalIllumination.ScreenSpaceRayCount = 1;
        settings.GlobalIllumination.ScreenSpaceStepCount = 64;
        settings.GlobalIllumination.ScreenSpaceHiZStartMip = 2;
        settings.GlobalIllumination.ScreenSpaceBinaryRefinementSteps = 3;
        settings.GlobalIllumination.ScreenSpaceMaximumRayDistance = 10.0f;
        settings.GlobalIllumination.ScreenSpaceThickness = 0.1f;
        settings.GlobalIllumination.ScreenSpaceEdgeFade = 0.05f;
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
            { "screenspaceao", 12 }, { "12", 12 },
            { "viewdepth", 13 }, { "depth", 13 }, { "13", 13 },
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
        return ViewOverrides == other.ViewOverrides && PipelineType == other.PipelineType && PipelineProfile == other.PipelineProfile &&
            PipelineFeatures == other.PipelineFeatures && Clouds == other.Clouds && Backend == other.Backend && Path == other.Path && AmbientOcclusion == other.AmbientOcclusion &&
            Reflections == other.Reflections && bGpuOcclusionEnabled == other.bGpuOcclusionEnabled &&
            bGpuBatchingEnabled == other.bGpuBatchingEnabled &&
            bGpuTightFrustumEnabled == other.bGpuTightFrustumEnabled &&
            bGpuShadowBatchingEnabled == other.bGpuShadowBatchingEnabled &&
            bTransientTextureReuse == other.bTransientTextureReuse &&
            TransientCacheBudgetMiB == other.TransientCacheBudgetMiB &&
            TextureStreamingBudgetMiB == other.TextureStreamingBudgetMiB &&
            bLodEnabled == other.bLodEnabled && LodMaxPixelError == other.LodMaxPixelError &&
            bVSync == other.bVSync &&
            FramesInFlight == other.FramesInFlight &&
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
            Upscaling == other.Upscaling &&
            leftAa.bTaaOptimizedHistoryGather == rightAa.bTaaOptimizedHistoryGather &&
            leftAa.bTaaSharedNeighborhood == rightAa.bTaaSharedNeighborhood &&
            leftGi.Technique == rightGi.Technique &&
            leftGi.Denoiser == rightGi.Denoiser &&
            leftGi.UnsupportedFallback == rightGi.UnsupportedFallback &&
            leftGi.RenderScale == rightGi.RenderScale &&
            leftGi.SamplesPerPixel == rightGi.SamplesPerPixel &&
            leftGi.MaxBounces == rightGi.MaxBounces &&
            leftGi.AtrousIterations == rightGi.AtrousIterations &&
            leftGi.bSvgfOptimizedKernels == rightGi.bSvgfOptimizedKernels &&
            leftGi.bSsgiLocalReactiveBudget == rightGi.bSsgiLocalReactiveBudget &&
            leftGi.bSvgfSharedUncertainty == rightGi.bSvgfSharedUncertainty &&
            leftGi.bSsgiEnvironmentMis == rightGi.bSsgiEnvironmentMis &&
            leftGi.ScreenSpaceRayCount == rightGi.ScreenSpaceRayCount &&
            leftGi.ScreenSpaceStepCount == rightGi.ScreenSpaceStepCount &&
            leftGi.ScreenSpaceHiZStartMip == rightGi.ScreenSpaceHiZStartMip &&
            leftGi.ScreenSpaceBinaryRefinementSteps ==
                rightGi.ScreenSpaceBinaryRefinementSteps &&
            leftGi.ScreenSpaceMaximumRayDistance ==
                rightGi.ScreenSpaceMaximumRayDistance &&
            leftGi.ScreenSpaceThickness == rightGi.ScreenSpaceThickness &&
            leftGi.ScreenSpaceEdgeFade == rightGi.ScreenSpaceEdgeFade &&
            leftGi.ScreenSpaceDiffuseHitDistanceWeight == rightGi.ScreenSpaceDiffuseHitDistanceWeight &&
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
            { RenderSettingIds::ViewOverrides, settings.ViewOverrides },
            { RenderSettingIds::PipelineType, settings.PipelineType },
            { RenderSettingIds::PipelineProfile, settings.PipelineProfile },
            { RenderSettingIds::PipelineFeatures, settings.PipelineFeatures },
            { RenderSettingIds::Backend, ToString(settings.Backend) },
            { RenderSettingIds::Path, ToString(settings.Path) },
            { RenderSettingIds::RenderHeight, settings.PostProcess.RenderHeight },
            { RenderSettingIds::TextureStreamingBudget, settings.TextureStreamingBudgetMiB },
            { RenderSettingIds::UpscalingMode, ToString(settings.Upscaling.Mode) },
            { RenderSettingIds::UpscalingQuality, ToString(settings.Upscaling.Quality) },
            { RenderSettingIds::UpscalingSharpness, settings.Upscaling.Sharpness },
            { RenderSettingIds::CloudQuality, std::array{ "Off", "Low", "Balanced", "High", "Custom" }.at(static_cast<size_t>(settings.Clouds.Quality)) },
            { RenderSettingIds::CloudRenderScale, settings.Clouds.RenderScale },
            { RenderSettingIds::CloudMaxSteps, settings.Clouds.MaxSteps },
            { RenderSettingIds::CloudLightSteps, settings.Clouds.LightSteps },
            { RenderSettingIds::CloudHistoryWeight, settings.Clouds.HistoryWeight },
            { RenderSettingIds::CloudDetailDistance, settings.Clouds.DetailDistance },
            { RenderSettingIds::CloudTemporal, settings.Clouds.bTemporal },
            { RenderSettingIds::CloudDebugView, settings.Clouds.DebugView },
            { RenderSettingIds::AoEnabled, settings.AmbientOcclusion.bEnabled },
            { RenderSettingIds::AoTechnique, settings.AmbientOcclusion.Technique == SceneAmbientOcclusionTechnique::SSAO ? "SSAO" : "GTAO" },
            { RenderSettingIds::AoNormalBias, settings.AmbientOcclusion.NormalBias },
            { RenderSettingIds::AoSampleCount, settings.AmbientOcclusion.SampleCount },
            { RenderSettingIds::AoFastUpsample, settings.AmbientOcclusion.bFastUpsample },
            { RenderSettingIds::GpuOcclusionEnabled, settings.bGpuOcclusionEnabled },
            { RenderSettingIds::GpuBatchingEnabled, settings.bGpuBatchingEnabled },
            { RenderSettingIds::GpuTightFrustumEnabled, settings.bGpuTightFrustumEnabled },
            { RenderSettingIds::GpuShadowBatchingEnabled, settings.bGpuShadowBatchingEnabled },
            { RenderSettingIds::TransientTextureReuse, settings.bTransientTextureReuse },
            { RenderSettingIds::TransientCacheBudgetMiB, settings.TransientCacheBudgetMiB },
            { RenderSettingIds::LodEnabled, settings.bLodEnabled },
            { RenderSettingIds::LodMaxPixelError, settings.LodMaxPixelError },
            { RenderSettingIds::AoRadius, settings.AmbientOcclusion.Radius },
            { RenderSettingIds::AoIntensity, settings.AmbientOcclusion.Intensity },
            { RenderSettingIds::AoSliceCount, settings.AmbientOcclusion.SliceCount },
            { RenderSettingIds::AoStepsPerSide, settings.AmbientOcclusion.StepsPerSide },
            { RenderSettingIds::SsrGuideCache, settings.Reflections.bGuideCacheEnabled },
            { RenderSettingIds::SsrEnabled, settings.Reflections.bScreenSpaceEnabled },
            { RenderSettingIds::ReflectionProbesEnabled, settings.Reflections.bLocalProbesEnabled },
            { RenderSettingIds::SsrMaximumDistance, settings.Reflections.MaximumDistance },
            { RenderSettingIds::SsrThickness, settings.Reflections.Thickness },
            { RenderSettingIds::SsrRoughnessCutoff, settings.Reflections.RoughnessCutoff },
            { RenderSettingIds::SsrEdgeFade, settings.Reflections.EdgeFade },
            { RenderSettingIds::SsrNormalBias, settings.Reflections.NormalBias },
            { RenderSettingIds::SsrStepCount, settings.Reflections.StepCount },
            { RenderSettingIds::SsrBinaryRefinementSteps, settings.Reflections.BinaryRefinementSteps },
            { RenderSettingIds::VSync, settings.bVSync },
            { RenderSettingIds::FramesInFlight, settings.FramesInFlight },
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
            { RenderSettingIds::TaaOptimizedHistoryGather,
                settings.AntiAliasing.bTaaOptimizedHistoryGather },
            { RenderSettingIds::TaaSharedNeighborhood,
                settings.AntiAliasing.bTaaSharedNeighborhood },
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
            { RenderSettingIds::GiSsgiEnvironmentMis, settings.GlobalIllumination.bSsgiEnvironmentMis },
            { RenderSettingIds::GiSvgfOptimizedKernels,
                settings.GlobalIllumination.bSvgfOptimizedKernels },
            { RenderSettingIds::GiSsgiLocalReactiveBudget,
                settings.GlobalIllumination.bSsgiLocalReactiveBudget },
            { RenderSettingIds::GiSvgfSharedUncertainty,
                settings.GlobalIllumination.bSvgfSharedUncertainty },
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
            { RenderSettingIds::GiSsgiDiffuseHitDistanceWeight,
                settings.GlobalIllumination.ScreenSpaceDiffuseHitDistanceWeight },
            { RenderSettingIds::GiSsgiTemporalWeight,
                settings.GlobalIllumination.ScreenSpaceTemporalWeight },
            { RenderSettingIds::PostProcessExposure,
                -std::log2(std::max(settings.PostProcess.Exposure, std::exp2(-32.0f))) },
            { RenderSettingIds::BloomIntensity, settings.PostProcess.BloomIntensity },
            { RenderSettingIds::ExposureCompensationEV, settings.PostProcess.ExposureCompensationEV },
            { RenderSettingIds::BloomThreshold, settings.PostProcess.BloomThreshold },
            { RenderSettingIds::AutoExposure, settings.PostProcess.bAutoExposure },
            { RenderSettingIds::ExposureKey, settings.PostProcess.ExposureKey },
            { RenderSettingIds::MinimumExposure, -std::log2(settings.PostProcess.MaximumExposure) },
            { RenderSettingIds::MaximumExposure, -std::log2(settings.PostProcess.MinimumExposure) },
            { RenderSettingIds::ExposureAdaptationSpeed, settings.PostProcess.ExposureAdaptationSpeed },
            { RenderSettingIds::ColorContrast, settings.PostProcess.Contrast },
            { RenderSettingIds::ColorSaturation, settings.PostProcess.Saturation },
            { RenderSettingIds::ColorTemperature, settings.PostProcess.Temperature },
            { RenderSettingIds::LocalExposureShadowStrength, settings.PostProcess.LocalExposureShadowStrength },
            { RenderSettingIds::LocalExposureHighlightStrength, settings.PostProcess.LocalExposureHighlightStrength },
            { RenderSettingIds::LocalExposureMaxEV, settings.PostProcess.LocalExposureMaxEV },
            { RenderSettingIds::SkinSubsurfaceEnabled, settings.PostProcess.SkinSubsurfaceEnabled },
            { RenderSettingIds::SkinSubsurfaceRadiusMeters, settings.PostProcess.SkinSubsurfaceRadiusMeters },
            { RenderSettingIds::SkinSubsurfaceQuality, settings.PostProcess.SkinSubsurfaceQuality },
            { RenderSettingIds::ShadowsEnabled, settings.Shadows.bEnabled },
            { RenderSettingIds::ContinuousPcfEnabled, settings.Shadows.bContinuousPcfEnabled },
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
            if (id == RenderSettingIds::ViewOverrides)
            {
                for (const auto& [view, patch] : value.items())
                {
                    if (!result.ViewOverrides.contains(view)) result.ViewOverrides[view] = Json::object();
                    result.ViewOverrides[view].update(patch);
                }
            }
            else if (id == RenderSettingIds::PipelineType)
                result.PipelineType = value.get<std::string>();
            else if (id == RenderSettingIds::PipelineProfile)
                result.PipelineProfile = value.get<std::string>();
            else if (id == RenderSettingIds::PipelineFeatures)
                result.PipelineFeatures = value;
            else if (id == RenderSettingIds::Backend)
                result.Backend = ParseRenderBackend(value.get<std::string>());
            else if (id == RenderSettingIds::Path)
                result.Path = ParseRenderingPath(value.get<std::string>());
            else if (id == RenderSettingIds::GpuOcclusionEnabled)
                result.bGpuOcclusionEnabled = value.get<bool>();
            else if (id == RenderSettingIds::GpuTightFrustumEnabled)
                result.bGpuTightFrustumEnabled = value.get<bool>();
            else if (id == RenderSettingIds::GpuBatchingEnabled)
                result.bGpuBatchingEnabled = value.get<bool>();
            else if (id == RenderSettingIds::GpuShadowBatchingEnabled)
                result.bGpuShadowBatchingEnabled = value.get<bool>();
            else if (id == RenderSettingIds::TransientCacheBudgetMiB)
                result.TransientCacheBudgetMiB = value.get<std::uint32_t>();
            else if (id == RenderSettingIds::TransientTextureReuse)
                result.bTransientTextureReuse = value.get<bool>();
            else if (id == RenderSettingIds::LodEnabled)
                result.bLodEnabled = value.get<bool>();
            else if (id == RenderSettingIds::LodMaxPixelError)
                result.LodMaxPixelError = value.get<float>();
            else if (id == RenderSettingIds::AoEnabled)
                result.AmbientOcclusion.bEnabled = value.get<bool>();
            else if (id == RenderSettingIds::AoTechnique)
                result.AmbientOcclusion.Technique = value.get<std::string>() == "SSAO"
                    ? SceneAmbientOcclusionTechnique::SSAO : SceneAmbientOcclusionTechnique::GTAO;
            else if (id == RenderSettingIds::AoNormalBias)
                result.AmbientOcclusion.NormalBias = value.get<float>();
            else if (id == RenderSettingIds::AoSampleCount)
                result.AmbientOcclusion.SampleCount = value.get<uint32_t>();
            else if (id == RenderSettingIds::CloudQuality)
            {
                const std::array<std::string, 5> cloudNames{ "Off", "Low", "Balanced", "High", "Custom" };
                result.Clouds.Quality = static_cast<CloudQuality>(std::find(cloudNames.begin(), cloudNames.end(), value.get<std::string>()) - cloudNames.begin());
            }
            else if (id == RenderSettingIds::CloudRenderScale)
                result.Clouds.RenderScale = value.get<float>();
            else if (id == RenderSettingIds::RenderHeight)
                result.PostProcess.RenderHeight = value.get<uint32_t>();
            else if (id == RenderSettingIds::TextureStreamingBudget)
                result.TextureStreamingBudgetMiB = value.get<uint32_t>();
            else if (id == RenderSettingIds::UpscalingMode)
                result.Upscaling.Mode = ParseUpscalingMode(value.get<std::string>());
            else if (id == RenderSettingIds::UpscalingQuality)
                result.Upscaling.Quality = ParseUpscalingQuality(value.get<std::string>());
            else if (id == RenderSettingIds::UpscalingSharpness)
                result.Upscaling.Sharpness = value.get<float>();
            else if (id == RenderSettingIds::CloudMaxSteps)
                result.Clouds.MaxSteps = value.get<uint32_t>();
            else if (id == RenderSettingIds::CloudLightSteps)
                result.Clouds.LightSteps = value.get<uint32_t>();
            else if (id == RenderSettingIds::CloudHistoryWeight)
                result.Clouds.HistoryWeight = value.get<float>();
            else if (id == RenderSettingIds::CloudDetailDistance)
                result.Clouds.DetailDistance = value.get<float>();
            else if (id == RenderSettingIds::CloudTemporal)
                result.Clouds.bTemporal = value.get<bool>();
            else if (id == RenderSettingIds::CloudDebugView)
                result.Clouds.DebugView = value.get<uint32_t>();
            else if (id == RenderSettingIds::AoFastUpsample)
                result.AmbientOcclusion.bFastUpsample = value.get<bool>();
            else if (id == RenderSettingIds::AoRadius)
                result.AmbientOcclusion.Radius = value.get<float>();
            else if (id == RenderSettingIds::AoIntensity)
                result.AmbientOcclusion.Intensity = value.get<float>();
            else if (id == RenderSettingIds::AoSliceCount)
                result.AmbientOcclusion.SliceCount = value.get<uint32_t>();
            else if (id == RenderSettingIds::AoStepsPerSide)
                result.AmbientOcclusion.StepsPerSide = value.get<uint32_t>();
            else if (id == RenderSettingIds::SsrGuideCache)
                result.Reflections.bGuideCacheEnabled = value.get<bool>();
            else if (id == RenderSettingIds::SsrEnabled)
                result.Reflections.bScreenSpaceEnabled = value.get<bool>();
            else if (id == RenderSettingIds::ReflectionProbesEnabled)
                result.Reflections.bLocalProbesEnabled = value.get<bool>();
            else if (id == RenderSettingIds::SsrMaximumDistance)
                result.Reflections.MaximumDistance = value.get<float>();
            else if (id == RenderSettingIds::SsrThickness)
                result.Reflections.Thickness = value.get<float>();
            else if (id == RenderSettingIds::SsrRoughnessCutoff)
                result.Reflections.RoughnessCutoff = value.get<float>();
            else if (id == RenderSettingIds::SsrEdgeFade)
                result.Reflections.EdgeFade = value.get<float>();
            else if (id == RenderSettingIds::SsrNormalBias)
                result.Reflections.NormalBias = value.get<float>();
            else if (id == RenderSettingIds::SsrStepCount)
                result.Reflections.StepCount = value.get<uint32_t>();
            else if (id == RenderSettingIds::SsrBinaryRefinementSteps)
                result.Reflections.BinaryRefinementSteps = value.get<uint32_t>();
            else if (id == RenderSettingIds::FramesInFlight)
                result.FramesInFlight = value.get<uint32_t>();
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
            else if (id == RenderSettingIds::TaaOptimizedHistoryGather)
                result.AntiAliasing.bTaaOptimizedHistoryGather = value.get<bool>();
            else if (id == RenderSettingIds::TaaSharedNeighborhood)
                result.AntiAliasing.bTaaSharedNeighborhood = value.get<bool>();
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
            else if (id == RenderSettingIds::GiSsgiEnvironmentMis)
                result.GlobalIllumination.bSsgiEnvironmentMis = value.get<bool>();
            else if (id == RenderSettingIds::GiSvgfOptimizedKernels)
                result.GlobalIllumination.bSvgfOptimizedKernels = value.get<bool>();
            else if (id == RenderSettingIds::GiSsgiLocalReactiveBudget)
                result.GlobalIllumination.bSsgiLocalReactiveBudget = value.get<bool>();
            else if (id == RenderSettingIds::GiSvgfSharedUncertainty)
                result.GlobalIllumination.bSvgfSharedUncertainty = value.get<bool>();
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
            else if (id == RenderSettingIds::GiSsgiDiffuseHitDistanceWeight)
                result.GlobalIllumination.ScreenSpaceDiffuseHitDistanceWeight = value.get<float>();
            else if (id == RenderSettingIds::GiSsgiTemporalWeight)
                result.GlobalIllumination.ScreenSpaceTemporalWeight = value.get<float>();
            else if (id == RenderSettingIds::PostProcessExposure)
                result.PostProcess.Exposure = Photometry::ExposureMultiplier(value.get<float>());
            else if (id == RenderSettingIds::BloomIntensity)
                result.PostProcess.BloomIntensity = value.get<float>();
            else if (id == RenderSettingIds::ExposureCompensationEV)
                result.PostProcess.ExposureCompensationEV = value.get<float>();
            else if (id == RenderSettingIds::BloomThreshold)
                result.PostProcess.BloomThreshold = value.get<float>();
            else if (id == RenderSettingIds::AutoExposure)
                result.PostProcess.bAutoExposure = value.get<bool>();
            else if (id == RenderSettingIds::ExposureKey)
                result.PostProcess.ExposureKey = value.get<float>();
            else if (id == RenderSettingIds::MinimumExposure)
                result.PostProcess.MaximumExposure = Photometry::ExposureMultiplier(value.get<float>());
            else if (id == RenderSettingIds::MaximumExposure)
                result.PostProcess.MinimumExposure = Photometry::ExposureMultiplier(value.get<float>());
            else if (id == RenderSettingIds::ExposureAdaptationSpeed)
                result.PostProcess.ExposureAdaptationSpeed = value.get<float>();
            else if (id == RenderSettingIds::ColorContrast)
                result.PostProcess.Contrast = value.get<float>();
            else if (id == RenderSettingIds::ColorSaturation)
                result.PostProcess.Saturation = value.get<float>();
            else if (id == RenderSettingIds::ColorTemperature)
                result.PostProcess.Temperature = value.get<float>();
            else if (id == RenderSettingIds::LocalExposureShadowStrength)
                result.PostProcess.LocalExposureShadowStrength = value.get<float>();
            else if (id == RenderSettingIds::LocalExposureHighlightStrength)
                result.PostProcess.LocalExposureHighlightStrength = value.get<float>();
            else if (id == RenderSettingIds::LocalExposureMaxEV)
                result.PostProcess.LocalExposureMaxEV = value.get<float>();
            else if (id == RenderSettingIds::SkinSubsurfaceEnabled)
                result.PostProcess.SkinSubsurfaceEnabled = value.get<bool>();
            else if (id == RenderSettingIds::SkinSubsurfaceRadiusMeters)
                result.PostProcess.SkinSubsurfaceRadiusMeters = value.get<float>();
            else if (id == RenderSettingIds::SkinSubsurfaceQuality)
                result.PostProcess.SkinSubsurfaceQuality = value.get<int>();
            else if (id == RenderSettingIds::ShadowsEnabled)
                result.Shadows.bEnabled = value.get<bool>();
            else if (id == RenderSettingIds::ContinuousPcfEnabled)
                result.Shadows.bContinuousPcfEnabled = value.get<bool>();
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
            std::pair{ RenderSettingIds::TaaOptimizedHistoryGather, "TaaOptimizedHistoryGather"sv },
            std::pair{ RenderSettingIds::TaaSharedNeighborhood, "TaaSharedNeighborhood"sv },
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
            std::pair{ RenderSettingIds::GiSsgiEnvironmentMis, "SsgiEnvironmentMis"sv },
            std::pair{ RenderSettingIds::GiSvgfOptimizedKernels, "SvgfOptimizedKernels"sv },
            std::pair{ RenderSettingIds::GiSsgiLocalReactiveBudget, "SsgiLocalReactiveBudget"sv },
            std::pair{ RenderSettingIds::GiSvgfSharedUncertainty, "SvgfSharedUncertainty"sv },
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
            std::pair{ RenderSettingIds::GiSsgiDiffuseHitDistanceWeight, "DiffuseHitDistanceWeight"sv },
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
        const bool countedDrawsAvailable = SupportsCountedIndirectDraws(capabilities.Render);
        const std::string countedDrawsReason = countedDrawsAvailable
            ? "GPU compacted draw groups are available."
            : "Counted indirect draws are unavailable; individual GPU-culled indirect draws are used.";
        for (const auto id : { RenderSettingIds::GpuBatchingEnabled, RenderSettingIds::GpuShadowBatchingEnabled })
            result.Availability[std::string(id)] = { countedDrawsAvailable, countedDrawsReason };
        if (!countedDrawsAvailable)
        {
            result.Effective.bGpuBatchingEnabled = false;
            result.Effective.bGpuShadowBatchingEnabled = false;
            if (requested.bGpuBatchingEnabled)
                AddDiagnostic(result.Diagnostics, RenderSettingIds::GpuBatchingEnabled, true, false,
                    RenderSettingDiagnosticSeverity::Warning, "capability_fallback", countedDrawsReason);
            if (requested.bGpuShadowBatchingEnabled)
                AddDiagnostic(result.Diagnostics, RenderSettingIds::GpuShadowBatchingEnabled, true, false,
                    RenderSettingDiagnosticSeverity::Warning, "capability_fallback", countedDrawsReason);
        }
        const bool skinAvailable = requested.Path == RenderingPath::Deferred &&
            !capabilities.bExternalViewportRendererActive;
        result.Availability[std::string(RenderSettingIds::SkinSubsurfaceEnabled)] = {
            skinAvailable, skinAvailable ? "Deferred skin diffusion is available."
                : "Skin diffusion requires the native deferred rendering path." };
        if (requested.PostProcess.SkinSubsurfaceEnabled && !skinAvailable)
        {
            result.Effective.PostProcess.SkinSubsurfaceEnabled = false;
            AddDiagnostic(result.Diagnostics, RenderSettingIds::SkinSubsurfaceEnabled, true, false,
                RenderSettingDiagnosticSeverity::Warning, "capability_fallback",
                "Skin diffusion is inactive outside native deferred rendering; the user request is preserved.");
        }
        result.Effective.Clouds = ResolveCloudSettings(requested.Clouds);
        for (const auto id : { RenderSettingIds::CloudRenderScale,
            RenderSettingIds::CloudMaxSteps, RenderSettingIds::CloudLightSteps })
        {
            const bool custom = requested.Clouds.Quality == CloudQuality::Custom;
            result.Availability[std::string(id)] = { custom,
                custom ? "Custom cloud budget is editable." :
                "Controlled by the cloud quality preset; select Custom to edit." };
        }
        result.Debug = debug;
        result.Capture = capture;
        result.Capabilities = capabilities;
        const auto featureTypes = EnumerateRenderFeatures();
        for (auto& feature : result.Effective.PipelineFeatures)
        {
            const std::string type = feature.value("type", "");
            const auto descriptor = std::ranges::find(featureTypes, type, &RenderFeatureDescriptor::Type);
            if (descriptor == featureTypes.end() || feature.value("version", 0u) != descriptor->Version)
            {
                const Json original = feature;
                feature["enabled"] = false;
                AddDiagnostic(result.Diagnostics, RenderSettingIds::PipelineFeatures, original, feature,
                    RenderSettingDiagnosticSeverity::Warning, "feature_unavailable",
                    "Feature '" + type + "' is missing or has an unsupported configuration version; requested payload preserved.");
            }
        }
        if (!IsRenderPipelineRegistered(requested.PipelineType))
        {
            result.Effective.PipelineType = "realtime";
            AddDiagnostic(result.Diagnostics, RenderSettingIds::PipelineType, requested.PipelineType, "realtime",
                RenderSettingDiagnosticSeverity::Warning, "pipeline_unavailable",
                "Requested pipeline is unavailable; realtime is active and the request is preserved.");
        }

        if (requested.GlobalIllumination.Denoiser != GlobalIlluminationDenoiserKind::Svgf &&
            result.Debug.GiDebugView == GlobalIlluminationDebugView::TemporalDiffuseIllumination)
        {
            result.Debug.GiDebugView = GlobalIlluminationDebugView::RawIndirectDiffuse;
            AddDiagnostic(result.Diagnostics, RenderSettingIds::GiDebugView,
                "TemporalDiffuseIllumination", "RawIndirectDiffuse",
                RenderSettingDiagnosticSeverity::Warning, "unavailable_signal",
                "Temporal illumination requires SVGF; the effective view is raw material-weighted radiance.");
        }

        const RenderSettingAvailability backendAvailability =
            GetRenderBackendAvailability(requested.Backend, capabilities);
        const bool bBackendAvailable = backendAvailability.bAvailable;
        result.Availability[std::string(RenderSettingIds::Backend)] = {
            bBackendAvailable,
            backendAvailability.Reason };
        if (!bBackendAvailable)
        {
            result.Effective.Backend = IsRenderBackendCompiled(RenderBackend::DX12)
                ? RenderBackend::DX12 : RenderBackend::Vulkan;
            AddDiagnostic(result.Diagnostics, RenderSettingIds::Backend,
                ToString(requested.Backend), ToString(result.Effective.Backend),
                RenderSettingDiagnosticSeverity::Warning,
                "capability_fallback",
                backendAvailability.Reason +
                    " " + ToString(result.Effective.Backend) + " is used without changing the request.");
        }

        if (HasAaCapabilities(capabilities.AntiAliasing))
        {
            AntiAliasingSelection aa = ResolveAntiAliasingSelection(
                requested.AntiAliasing, capabilities.AntiAliasing,
                capabilities.AntiAliasingPath);
            if (aa.Effective.Mode == AntiAliasingMode::Msaa &&
                (requested.Path == RenderingPath::Deferred || requested.bGpuOcclusionEnabled ||
                    requested.Reflections.bScreenSpaceEnabled || requested.Reflections.bLocalProbesEnabled))
            {
                aa.Effective.Mode = AntiAliasingMode::Taa;
                aa.bFallback = true;
                aa.bRequestedModeAvailable = false;
                aa.Reason = "Deferred, reflection composition and same-frame GPU occlusion use native-resolution TAA; Forward MSAA requires reflection passes and GPU occlusion disabled.";
            }
            if (!aa.bBypassed &&
                result.Debug.GiDebugView != GlobalIlluminationDebugView::Composite)
            {
                aa.Effective.Mode = AntiAliasingMode::None;
                aa.bFallback = true;
                aa.bRequestedModeAvailable = false;
                aa.Reason = "Anti-aliasing is disabled for GI diagnostic views to preserve data and sample positions.";
            }
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

    ResolvedRenderSettings ResolveRenderViewSettings(
        const ResolvedRenderSettings& base, const Json& worldOverrides, const Json& cameraOverrides)
    {
        auto requested = base.Requested;
        auto sources = base.Sources;
        std::vector<RenderSettingDiagnostic> diagnostics;
        const auto apply = [&](SettingsLayer layer, const Json& patch)
        {
            if (!patch.is_object())
            {
                AddDiagnostic(diagnostics, "Rendering.Views", patch, {}, RenderSettingDiagnosticSeverity::Error,
                    "invalid_override", "View overrides must be an object.");
                return;
            }
            Json allowed = Json::object();
            for (const auto& [id, value] : patch.items())
            {
                const auto* descriptor = FindRenderSettingDescriptor(id);
                if (!descriptor || !descriptor->bPersistedRenderSetting ||
                    std::ranges::find(descriptor->WritableLayers, layer) == descriptor->WritableLayers.end())
                {
                    AddDiagnostic(diagnostics, id, value, {}, RenderSettingDiagnosticSeverity::Error,
                        "layer_not_writable", "This setting cannot be overridden by a world or camera.");
                    continue;
                }
                if (sources.contains(id) && sources.at(id) == SettingsLayer::CommandLine) continue;
                if (!value.is_null()) { allowed[id] = value; sources[id] = layer; }
            }
            requested = ApplyRenderSettingsFlatValues(requested, allowed, diagnostics);
        };
        apply(SettingsLayer::World, worldOverrides);
        apply(SettingsLayer::Camera, cameraOverrides);
        auto result = ResolveRenderSettings(requested, base.Debug, base.Capture, base.Capabilities);
        result.Sources = std::move(sources);
        result.EffectiveSources = result.Sources;
        const auto requestedValues = RenderSettingsToFlatValues(result.Requested);
        const auto effectiveValues = RenderSettingsToFlatValues(result.Effective);
        for (const auto& [id, value] : effectiveValues.items())
            if (requestedValues.at(id) != value) result.EffectiveSources[id] = SettingsLayer::Capability;
        result.Diagnostics.insert(result.Diagnostics.end(), diagnostics.begin(), diagnostics.end());
        result.Revision = base.Revision;
        result.Hash = HashResolvedRenderSettings(result);
        return result;
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
            {
                if (id.starts_with("Rendering.Clouds."))
                {
                    const auto& previous = before.Effective.Clouds;
                    const auto& next = after.Effective.Clouds;
                    const bool wasActive = previous.Quality != CloudQuality::Off;
                    const bool active = next.Quality != CloudQuality::Off;
                    impacts.insert(previous == next || (!wasActive && !active) ? ApplyImpact::Live :
                        wasActive != active || previous.RenderScale != next.RenderScale ?
                            ApplyImpact::RenderTargetRecreate : ApplyImpact::TemporalHistoryReset);
                }
                else
                    impacts.insert(descriptor->Impact);
            }
        }
        // A capability change can alter effective draw grouping while keeping
        // the user's request intact. The recorded graph must follow that change.
        for (const auto& [id, changed] : {
            std::pair{ RenderSettingIds::GpuBatchingEnabled,
                before.Effective.bGpuBatchingEnabled != after.Effective.bGpuBatchingEnabled },
            std::pair{ RenderSettingIds::GpuShadowBatchingEnabled,
                before.Effective.bGpuShadowBatchingEnabled != after.Effective.bGpuShadowBatchingEnabled } })
        {
            if (!changed)
                continue;
            if (std::find(result.ChangedPropertyIds.begin(), result.ChangedPropertyIds.end(), id) == result.ChangedPropertyIds.end())
                result.ChangedPropertyIds.emplace_back(id);
            impacts.insert(ApplyImpact::RenderGraphRebuild);
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
        // Scene graph/target candidates retire behind their actual submissions.
        // Only the swap-chain owner requires a device-idle boundary here.
        result.bGpuIdleRequired = result.bSwapchainRecreateRequired;
        return result;
    }

    Json GetGlobalIlluminationQualityPresetPatch(Preset preset,
        std::optional<GlobalIlluminationTechniqueKind> technique)
    {
        switch (preset)
        {
        case Preset::Low:
            return Json{
                { RenderSettingIds::GiRenderScale, 0.5 },
                { RenderSettingIds::GiMaxBounces, 1 },
                { RenderSettingIds::GiAtrousIterations, 2 },
                { RenderSettingIds::GiSvgfOptimizedKernels, true },
                { RenderSettingIds::GiBilateralUpsample, true },
                { RenderSettingIds::GiSsgiRayCount,
                    technique == GlobalIlluminationTechniqueKind::ScreenSpace ? 2 : 1 },
                { RenderSettingIds::GiSsgiStepCount, 64 },
                { RenderSettingIds::GiSsgiHiZStartMip, 2 },
                { RenderSettingIds::GiSsgiBinaryRefinementSteps, 3 },
                { RenderSettingIds::GiSsgiMaximumRayDistance, 10.0 },
                { RenderSettingIds::GiSsgiThickness, 0.1f },
                { RenderSettingIds::GiSsgiEdgeFade, 0.05f },
                { RenderSettingIds::GiSsgiTemporalWeight, 0.97f },
            };
        case Preset::Balanced:
            return Json{
                { RenderSettingIds::GiRenderScale, 0.5 },
                { RenderSettingIds::GiMaxBounces, 1 },
                { RenderSettingIds::GiAtrousIterations, 3 },
                { RenderSettingIds::GiSvgfOptimizedKernels, true },
                { RenderSettingIds::GiBilateralUpsample, true },
                { RenderSettingIds::GiSsgiRayCount, 2 },
                { RenderSettingIds::GiSsgiStepCount, 96 },
                { RenderSettingIds::GiSsgiHiZStartMip, 2 },
                { RenderSettingIds::GiSsgiBinaryRefinementSteps, 4 },
                { RenderSettingIds::GiSsgiMaximumRayDistance, 10.0 },
                { RenderSettingIds::GiSsgiThickness, 0.1f },
                { RenderSettingIds::GiSsgiEdgeFade, 0.05f },
                { RenderSettingIds::GiSsgiTemporalWeight, 0.97f },
            };
        case Preset::High:
            // Screen-space transport uses a single bounce and retains half-resolution
            // tracing. Unspecified and other techniques preserve their existing budget.
            return Json{
                { RenderSettingIds::GiRenderScale,
                    technique == GlobalIlluminationTechniqueKind::ScreenSpace ? 0.5 : 1.0 },
                { RenderSettingIds::GiMaxBounces,
                    technique == GlobalIlluminationTechniqueKind::ScreenSpace ? 1 : 4 },
                { RenderSettingIds::GiAtrousIterations,
                    technique == GlobalIlluminationTechniqueKind::ScreenSpace ? 3 : 4 },
                { RenderSettingIds::GiSvgfOptimizedKernels, true },
                { RenderSettingIds::GiBilateralUpsample, true },
                { RenderSettingIds::GiSsgiRayCount,
                    technique == GlobalIlluminationTechniqueKind::ScreenSpace ? 4 : 8 },
                { RenderSettingIds::GiSsgiStepCount, 128 },
                { RenderSettingIds::GiSsgiHiZStartMip, 2 },
                { RenderSettingIds::GiSsgiBinaryRefinementSteps, 6 },
                { RenderSettingIds::GiSsgiMaximumRayDistance,
                    technique == GlobalIlluminationTechniqueKind::ScreenSpace ? 10.0 : 20.0 },
                { RenderSettingIds::GiSsgiThickness, 0.1f },
                { RenderSettingIds::GiSsgiEdgeFade, 0.05f },
                { RenderSettingIds::GiSsgiTemporalWeight, 0.97f },
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
            const Json patch = GetGlobalIlluminationQualityPresetPatch(
                preset, settings.GlobalIllumination.Technique);
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
