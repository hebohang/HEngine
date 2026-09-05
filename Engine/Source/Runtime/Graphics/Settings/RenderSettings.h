#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    inline constexpr std::string_view RenderSettingsSchema =
        "hengine.render-settings";
    inline constexpr std::uint32_t RenderSettingsSchemaVersion = 2;

    namespace RenderSettingIds
    {
        inline constexpr std::string_view Backend = "Rendering.Backend";
        inline constexpr std::string_view VSync = "Rendering.Presentation.VSync";
        inline constexpr std::string_view AntiAliasingMode =
            "Rendering.AntiAliasing.Mode";
        inline constexpr std::string_view MsaaSamples =
            "Rendering.AntiAliasing.MsaaSamples";
        inline constexpr std::string_view FxaaEdgeThreshold =
            "Rendering.AntiAliasing.FxaaEdgeThreshold";
        inline constexpr std::string_view FxaaEdgeThresholdMin =
            "Rendering.AntiAliasing.FxaaEdgeThresholdMin";
        inline constexpr std::string_view FxaaSubpixelQuality =
            "Rendering.AntiAliasing.FxaaSubpixelQuality";
        inline constexpr std::string_view SmaaEdgeThreshold =
            "Rendering.AntiAliasing.SmaaEdgeThreshold";
        inline constexpr std::string_view SmaaMaxSearchSteps =
            "Rendering.AntiAliasing.SmaaMaxSearchSteps";
        inline constexpr std::string_view SmaaMaxDiagonalSearchSteps =
            "Rendering.AntiAliasing.SmaaMaxDiagonalSearchSteps";
        inline constexpr std::string_view SmaaCornerRounding =
            "Rendering.AntiAliasing.SmaaCornerRounding";
        inline constexpr std::string_view TaaHistoryWeight =
            "Rendering.AntiAliasing.TaaHistoryWeight";
        inline constexpr std::string_view TaaSharpness =
            "Rendering.AntiAliasing.TaaSharpness";
        inline constexpr std::string_view GiTechnique =
            "Rendering.GI.Technique";
        inline constexpr std::string_view GiDenoiser =
            "Rendering.GI.Denoiser";
        inline constexpr std::string_view GiUnsupportedFallback =
            "Rendering.GI.UnsupportedFallback";
        inline constexpr std::string_view GiRenderScale =
            "Rendering.GI.RenderScale";
        inline constexpr std::string_view GiSamplesPerPixel =
            "Rendering.GI.SamplesPerPixel";
        inline constexpr std::string_view GiMaxBounces =
            "Rendering.GI.MaxBounces";
        inline constexpr std::string_view GiAtrousIterations =
            "Rendering.GI.AtrousIterations";
        inline constexpr std::string_view GiBilateralUpsample =
            "Rendering.GI.BilateralUpsample";
        inline constexpr std::string_view GiSsgiRayCount =
            "Rendering.GI.SSGI.RayCount";
        inline constexpr std::string_view GiSsgiStepCount =
            "Rendering.GI.SSGI.StepCount";
        inline constexpr std::string_view GiSsgiHiZStartMip =
            "Rendering.GI.SSGI.HiZStartMip";
        inline constexpr std::string_view GiSsgiBinaryRefinementSteps =
            "Rendering.GI.SSGI.BinaryRefinementSteps";
        inline constexpr std::string_view GiSsgiMaximumRayDistance =
            "Rendering.GI.SSGI.MaximumRayDistance";
        inline constexpr std::string_view GiSsgiThickness =
            "Rendering.GI.SSGI.Thickness";
        inline constexpr std::string_view GiSsgiEdgeFade =
            "Rendering.GI.SSGI.EdgeFade";
        inline constexpr std::string_view GiSsgiTemporalWeight =
            "Rendering.GI.SSGI.TemporalWeight";
        inline constexpr std::string_view PostProcessExposure =
            "Rendering.PostProcess.Exposure";
        inline constexpr std::string_view ShadowsEnabled =
            "Rendering.Shadows.Enabled";

        inline constexpr std::string_view GiDebugView =
            "Rendering.Debug.GI.View";
        inline constexpr std::string_view GiDirectOnly =
            "Rendering.Debug.GI.DirectOnly";
        inline constexpr std::string_view PbrDebugMode =
            "Rendering.Debug.PBR.Mode";
        inline constexpr std::string_view RenderGraphValidation =
            "Rendering.Debug.RenderGraph.Validation";
        inline constexpr std::string_view ShadowCascadeDebug =
            "Rendering.Debug.Shadows.CascadeCoverage";
        inline constexpr std::string_view CaptureSupersampleScale =
            "Rendering.Capture.SupersampleScale";
    }

    enum class SettingsLayer : std::uint8_t
    {
        EmergencyFallback,
        EngineDefault,
        Project,
        User,
        Session,
        CommandLine,
        World,
        Camera,
        Capability,
        Effective,
    };

    enum class RenderSettingValueType : std::uint8_t
    {
        Boolean,
        Integer,
        Float,
        Enumeration,
        String,
    };

    enum class RenderSettingDiagnosticSeverity : std::uint8_t
    {
        Info,
        Warning,
        Error,
    };

    enum class ApplyImpact : std::uint8_t
    {
        Live,
        TemporalHistoryReset,
        RenderGraphRebuild,
        RenderTargetRecreate,
        SwapchainRecreate,
        ProcessRestart,
        BuildReconfigure,
    };

    enum class GlobalIlluminationQualityPreset : std::uint8_t
    {
        Custom,
        Low,
        Balanced,
        High,
    };

    struct PostProcessSettings
    {
        float Exposure = 0.8f;

        [[nodiscard]] bool operator==(const PostProcessSettings&) const = default;
    };

    struct ShadowSettings
    {
        bool bEnabled = true;

        [[nodiscard]] bool operator==(const ShadowSettings&) const = default;
    };

    struct RenderDebugSettings
    {
        GlobalIlluminationDebugView GiDebugView =
            GlobalIlluminationDebugView::Composite;
        bool bGiDirectOnly = false;
        std::uint32_t PbrDebugMode = 0;
        bool bRenderGraphValidation = false;
        bool bShadowCascadeDebug = false;

        [[nodiscard]] bool operator==(const RenderDebugSettings&) const = default;
    };

    struct RenderCaptureSettings
    {
        std::uint32_t SupersampleScale = 1;

        [[nodiscard]] bool operator==(const RenderCaptureSettings&) const = default;
    };

    struct RenderSettings
    {
        RenderBackend Backend = RenderBackend::DX12;
        bool bVSync = false;
        AntiAliasingSettings AntiAliasing;
        GlobalIlluminationSettings GlobalIllumination;
        PostProcessSettings PostProcess;
        ShadowSettings Shadows;

        // Legacy renderer bridge structs still contain debug/capture members.
        // RenderSettings equality intentionally compares only persisted fields.
        [[nodiscard]] bool operator==(const RenderSettings& other) const noexcept;
    };

    struct RenderSettingDiagnostic
    {
        std::string PropertyId;
        Json InputValue;
        Json NormalizedValue;
        RenderSettingDiagnosticSeverity Severity =
            RenderSettingDiagnosticSeverity::Info;
        std::string Code;
        std::string Message;
    };

    struct RenderSettingAvailability
    {
        bool bAvailable = true;
        std::string Reason;
    };

    struct RenderSettingDescriptor
    {
        std::string PropertyId;
        RenderSettingValueType Type = RenderSettingValueType::String;
        Json DefaultValue;
        std::optional<double> Minimum;
        std::optional<double> Maximum;
        std::string Unit;
        std::string Category;
        std::string Subcategory;
        std::string LabelTextId;
        std::string DescriptionTextId;
        std::string EnglishFallback;
        std::vector<SettingsLayer> WritableLayers;
        std::vector<std::string> EnumValues;
        std::vector<GlobalIlluminationQualityPreset> PresetMembership;
        std::string CapabilityPredicate;
        ApplyImpact Impact = ApplyImpact::Live;
        bool bAdvanced = false;
        bool bRestartRequired = false;
        bool bPersistedRenderSetting = true;
    };

    struct RenderSettingsCapabilities
    {
        RenderCapabilities Render;
        // Empty entries fall back to the compiled-backend registry. Runtime
        // initialization may override an entry with a driver/device failure so
        // Requested and Effective remain distinct without rewriting settings.
        std::map<RenderBackend, RenderSettingAvailability> BackendAvailability;
        AntiAliasingCapabilities AntiAliasing;
        AntiAliasingRenderPath AntiAliasingPath =
            AntiAliasingRenderPath::RealtimeRaster;
        bool bScreenSpaceGiAvailable = true;
        bool bExternalViewportRendererAvailable = false;
        bool bExternalViewportRendererActive = false;
    };

    struct RenderSettingsChangePlan
    {
        std::vector<std::string> ChangedPropertyIds;
        std::vector<ApplyImpact> Impacts;
        ApplyImpact HighestImpact = ApplyImpact::Live;
        bool bGpuIdleRequired = false;
        bool bTemporalHistoryResetRequired = false;
        bool bRenderGraphRebuildRequired = false;
        bool bRenderTargetRecreateRequired = false;
        bool bSwapchainRecreateRequired = false;
        bool bProcessRestartRequired = false;
        bool bBuildReconfigureRequired = false;
    };

    struct ResolvedRenderSettings
    {
        RenderSettings Requested;
        RenderSettings Effective;
        RenderDebugSettings Debug;
        RenderCaptureSettings Capture;
        std::vector<RenderSettingDiagnostic> Diagnostics;
        std::map<std::string, SettingsLayer, std::less<>> Sources;
        std::map<std::string, SettingsLayer, std::less<>> EffectiveSources;
        std::map<std::string, RenderSettingAvailability, std::less<>> Availability;
        RenderSettingsCapabilities Capabilities;
        RenderSettingsChangePlan PendingImpact;
        std::uint64_t Revision = 0;
        std::string Hash;
    };

    struct RenderSettingsDraft
    {
        SettingsLayer TargetLayer = SettingsLayer::Session;
        std::uint64_t BaseRevision = 0;
        std::string BaseHash;
        // Stable English property IDs are protocol keys. A null value removes
        // that key from TargetLayer and restores inheritance.
        Json Patch = Json::object();
    };

    struct RenderSettingsApplyResult
    {
        bool bSuccess = false;
        bool bSaved = false;
        bool bRolledBack = false;
        std::string ErrorCode;
        std::string Message;
        std::uint64_t PreviousRevision = 0;
        std::uint64_t Revision = 0;
        std::string Hash;
        SettingsLayer Layer = SettingsLayer::Session;
        RenderSettingsChangePlan ChangePlan;
        std::vector<RenderSettingDiagnostic> Diagnostics;
    };

    struct RenderSettingsMigrationResult
    {
        bool bSuccess = true;
        bool bFutureSchema = false;
        bool bMigrated = false;
        std::uint32_t SourceVersion = 0;
        Json RenderValues = Json::object();
        Json DebugValues = Json::object();
        Json CaptureValues = Json::object();
        std::vector<RenderSettingDiagnostic> Diagnostics;
    };

    [[nodiscard]] const char* ToString(SettingsLayer layer) noexcept;
    [[nodiscard]] const char* ToString(ApplyImpact impact) noexcept;
    [[nodiscard]] const char* ToString(
        RenderSettingDiagnosticSeverity severity) noexcept;
    [[nodiscard]] const char* ToString(
        GlobalIlluminationQualityPreset preset) noexcept;

    [[nodiscard]] RenderSettings MakeEmergencyFallbackRenderSettings();
    [[nodiscard]] RenderDebugSettings
        ReadRenderDebugSettingsFromEnvironment();
    [[nodiscard]] const std::vector<RenderSettingDescriptor>&
        GetRenderSettingDescriptors();
    [[nodiscard]] const RenderSettingDescriptor* FindRenderSettingDescriptor(
        std::string_view propertyId) noexcept;
    [[nodiscard]] Json RenderSettingsToFlatValues(
        const RenderSettings& settings);
    [[nodiscard]] Json RenderDebugSettingsToFlatValues(
        const RenderDebugSettings& settings);
    [[nodiscard]] Json RenderCaptureSettingsToFlatValues(
        const RenderCaptureSettings& settings);
    [[nodiscard]] RenderSettings ApplyRenderSettingsFlatValues(
        const RenderSettings& base,
        const Json& values,
        std::vector<RenderSettingDiagnostic>& diagnostics);
    [[nodiscard]] RenderDebugSettings ApplyRenderDebugFlatValues(
        const RenderDebugSettings& base,
        const Json& values,
        std::vector<RenderSettingDiagnostic>& diagnostics);
    [[nodiscard]] RenderCaptureSettings ApplyRenderCaptureFlatValues(
        const RenderCaptureSettings& base,
        const Json& values,
        std::vector<RenderSettingDiagnostic>& diagnostics);
    [[nodiscard]] RenderSettingsMigrationResult MigrateRenderSettingsDocument(
        const Json& document);
    [[nodiscard]] Json SerializeRenderSettingsLayerDocument(
        const Json& renderValues,
        const Json& debugValues = Json::object(),
        const Json& captureValues = Json::object());
    [[nodiscard]] ResolvedRenderSettings ResolveRenderSettings(
        const RenderSettings& requested,
        const RenderDebugSettings& debug,
        const RenderCaptureSettings& capture,
        const RenderSettingsCapabilities& capabilities);

    [[nodiscard]] RenderSettingAvailability GetRenderBackendAvailability(
        RenderBackend backend,
        const RenderSettingsCapabilities& capabilities);
    [[nodiscard]] RenderSettingsChangePlan BuildRenderSettingsChangePlan(
        const ResolvedRenderSettings& before,
        const ResolvedRenderSettings& after);
    [[nodiscard]] Json GetGlobalIlluminationQualityPresetPatch(
        GlobalIlluminationQualityPreset preset);
    [[nodiscard]] GlobalIlluminationQualityPreset
        DetectGlobalIlluminationQualityPreset(const RenderSettings& settings);
    [[nodiscard]] std::string HashResolvedRenderSettings(
        const ResolvedRenderSettings& settings);
    [[nodiscard]] bool HasDiagnosticErrors(
        const std::vector<RenderSettingDiagnostic>& diagnostics) noexcept;
}
