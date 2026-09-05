#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/Graphics/Settings/RenderSettings.h"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace HEngine
{
    struct RenderSettingsServiceInit
    {
        std::filesystem::path EngineDefaultPath;
        std::filesystem::path ProjectOverridePath;
        std::filesystem::path UserOverridePath;
    };

    class RenderSettingsService final :
        public PublicSingleton<RenderSettingsService>
    {
    public:
        using RuntimeApplyCallback = std::function<void(
            const ResolvedRenderSettings& settings,
            const RenderSettingsChangePlan& plan)>;

        RenderSettingsService() = default;
        RenderSettingsService(const RenderSettingsService&) = delete;
        RenderSettingsService& operator=(const RenderSettingsService&) = delete;

        void Initialize(const RenderSettingsServiceInit& init);
        void Clear();
        [[nodiscard]] bool IsInitialized() const noexcept;

        [[nodiscard]] ResolvedRenderSettings GetSnapshot() const;
        [[nodiscard]] RenderSettingsDraft CreateDraft(
            SettingsLayer targetLayer = SettingsLayer::Session) const;
        [[nodiscard]] Json GetLayerValues(SettingsLayer layer) const;
        [[nodiscard]] RenderSettingsApplyResult ApplyDraft(
            const RenderSettingsDraft& draft,
            bool bPersist = true);
        [[nodiscard]] RenderSettingsApplyResult PlanDraft(
            const RenderSettingsDraft& draft) const;
        [[nodiscard]] RenderSettingsApplyResult ResetProperty(
            SettingsLayer layer,
            std::string_view propertyId,
            std::uint64_t expectedRevision,
            std::string_view expectedHash,
            bool bPersist = true);
        [[nodiscard]] RenderSettingsApplyResult ResetCategory(
            SettingsLayer layer,
            std::string_view categoryPrefix,
            std::uint64_t expectedRevision,
            std::string_view expectedHash,
            bool bPersist = true);
        [[nodiscard]] RenderSettingsApplyResult ResetLayer(
            SettingsLayer layer,
            std::uint64_t expectedRevision,
            std::string_view expectedHash,
            bool bPersist = true);
        [[nodiscard]] RenderSettingsApplyResult ResetToEngineDefault(
            std::uint64_t expectedRevision,
            std::string_view expectedHash);

        // Invalid external edits never replace the last-known-good layer.
        [[nodiscard]] bool ReloadExternalLayer(SettingsLayer layer);
        void SetCapabilities(RenderSettingsCapabilities capabilities);
        void SetRuntimeApplyCallback(RuntimeApplyCallback callback);
        void SetCommandLineOverrides(const Json& stableIdPatch);

        // World and camera overrides are resolved into a detached immutable
        // value snapshot; they never mutate Project/User/Session state.
        [[nodiscard]] ResolvedRenderSettings ResolveWithWorldCameraOverrides(
            const Json& worldOverrides,
            const Json& cameraOverrides) const;

        [[nodiscard]] GlobalIlluminationSettings
            BuildRequestedRuntimeGlobalIllumination() const;
        [[nodiscard]] AntiAliasingSettings
            BuildRequestedRuntimeAntiAliasing() const;

    private:
        struct LayerState
        {
            Json RenderValues = Json::object();
            Json DebugValues = Json::object();
            Json CaptureValues = Json::object();
            std::vector<RenderSettingDiagnostic> Diagnostics;
            bool bFutureSchema = false;
        };

        [[nodiscard]] LayerState LoadLayer(
            SettingsLayer layer,
            const std::filesystem::path& path,
            bool bRequired) const;
        [[nodiscard]] ResolvedRenderSettings ResolveLocked(
            const std::map<SettingsLayer, LayerState>& layers) const;
        void RefreshSnapshotLocked();
        [[nodiscard]] std::filesystem::path GetLayerPath(
            SettingsLayer layer) const;
        [[nodiscard]] Json BuildSparseLayerDocumentLocked(
            SettingsLayer layer,
            std::map<SettingsLayer, LayerState>& layers) const;
        [[nodiscard]] RenderSettingsApplyResult ApplyCandidateLocked(
            SettingsLayer layer,
            std::map<SettingsLayer, LayerState> candidateLayers,
            bool bPersist,
            std::uint64_t expectedRevision,
            std::string_view expectedHash);

        mutable std::mutex mMutex;
        RenderSettingsServiceInit mInit;
        std::map<SettingsLayer, LayerState> mLayers;
        RenderSettingsCapabilities mCapabilities;
        ResolvedRenderSettings mSnapshot;
        RuntimeApplyCallback mRuntimeApply;
        bool bInitialized = false;
    };

    [[nodiscard]] Json SerializeResolvedRenderSettings(
        const ResolvedRenderSettings& settings,
        bool bIncludeDescriptors = false);
    [[nodiscard]] Json SerializeRenderSettingsApplyResult(
        const RenderSettingsApplyResult& result);
}
