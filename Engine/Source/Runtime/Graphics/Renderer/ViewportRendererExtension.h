#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    class RayTracingScene;
    class ShaderManager;

    enum class OfflineViewportAov : uint8_t
    {
        Beauty,
        DirectDiffuse,
        IndirectDiffuse,
        Albedo,
        WorldNormal,
        ViewDepth,
        MaterialId,
        InstanceId,
        SampleCount,
    };

    enum class OfflineViewportSessionState : uint8_t
    {
        Idle,
        Rendering,
        Paused,
        Complete,
        Cancelled,
        Failed,
    };

    enum class OfflineViewportSeedMode : uint8_t
    {
        Fixed,
        Random,
    };

    enum class OfflineViewportOutputFormat : uint8_t
    {
        Pfm,
        ToneMappedPng,
        Raw,
    };

    struct OfflineViewportRendererSettings
    {
        uint32_t TargetSamples = 256;
        uint32_t SamplesPerFrame = 1;
        uint32_t MaxBounces = 8;
        float MaximumRayDistance = 1000.0f;
        float RenderScale = 1.0f;
        uint32_t FixedWidth = 0;
        uint32_t FixedHeight = 0;
        bool bLockAspectRatio = true;
        OfflineViewportSeedMode SeedMode = OfflineViewportSeedMode::Fixed;
        uint32_t Seed = 0x48454e47u;
        glm::vec3 EnvironmentRadiance{ 0.0f };
        bool bUseSceneEnvironment = true;
        bool bDirectOnly = false;
        bool bAutoReset = true;
        OfflineViewportAov PreviewAov = OfflineViewportAov::Beauty;
        std::filesystem::path OutputDirectory;
        std::string OutputFileName = "Reference";
        OfflineViewportOutputFormat OutputFormat =
            OfflineViewportOutputFormat::Pfm;
    };

    struct OfflineViewportRendererSupport
    {
        bool bSupported = false;
        std::string Reason;
    };

    struct OfflineViewportRendererProgress
    {
        OfflineViewportSessionState State = OfflineViewportSessionState::Idle;
        uint64_t AccumulatedSamples = 0;
        uint64_t TargetSamples = 0;
        double ElapsedSeconds = 0.0;
        double LastGpuMilliseconds = 0.0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        GlobalIlluminationHistoryResetReason LastResetReason =
            GlobalIlluminationHistoryResetReason::None;
        bool bSceneInvalidated = false;
        bool bCameraInvalidated = false;
        std::string Error;
    };

    struct OfflineViewportNativeAov
    {
        nvrhi::TextureHandle Texture;
        nvrhi::Format Format = nvrhi::Format::UNKNOWN;
        uint32_t Width = 0;
        uint32_t Height = 0;
        bool bSceneLinear = false;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(Texture) &&
                Format != nvrhi::Format::UNKNOWN &&
                Width > 0 && Height > 0;
        }
    };

    struct OfflineViewportRendererCreateContext
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        RayTracingScene* Scene = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        RenderCapabilities Capabilities;
        uint32_t FramesInFlight = 1;
    };

    class IOfflineViewportRendererSession :
        public IGlobalIlluminationTechnique
    {
    public:
        ~IOfflineViewportRendererSession() override = default;

        [[nodiscard]] virtual std::string_view GetProviderId() const noexcept = 0;
        [[nodiscard]] virtual const OfflineViewportRendererSettings&
            GetOfflineSettings() const noexcept = 0;
        virtual void ApplyOfflineSettings(
            const OfflineViewportRendererSettings& settings) = 0;
        [[nodiscard]] virtual OfflineViewportRendererProgress
            GetOfflineProgress() const = 0;
        virtual void SetLastGpuMilliseconds(double milliseconds) noexcept = 0;
        [[nodiscard]] virtual std::span<const OfflineViewportAov>
            GetAvailableAovs() const noexcept = 0;
        [[nodiscard]] virtual OfflineViewportNativeAov GetNativeAov(
            OfflineViewportAov aov) const = 0;

        virtual void Pause() noexcept = 0;
        virtual void Resume() noexcept = 0;
        virtual void Restart() = 0;
        virtual void Cancel() noexcept = 0;

        [[nodiscard]] virtual TextureReadbackHandle RequestExport(
            OfflineViewportAov aov,
            std::filesystem::path outputPath,
            OfflineViewportOutputFormat format) = 0;
        [[nodiscard]] virtual bool IsExportValid(
            TextureReadbackHandle handle) const noexcept = 0;
        [[nodiscard]] virtual TextureReadbackResult GetExportResult(
            TextureReadbackHandle handle) const = 0;
        virtual void ReleaseExport(TextureReadbackHandle handle) = 0;
    };

    using OfflineViewportRendererFactory = std::function<
        Scope<IOfflineViewportRendererSession>(
            const OfflineViewportRendererCreateContext&)>;
    using OfflineViewportRendererSupportQuery = std::function<
        OfflineViewportRendererSupport(
            const RenderCapabilities&,
            const std::filesystem::path&)>;

    struct OfflineViewportRendererProviderDescriptor
    {
        std::string Id;
        std::string DisplayName;
        std::string OwnerPluginId;
        int32_t SortOrder = 0;
        GlobalIlluminationTechniqueCapabilityRequirements
            CapabilityRequirements =
                GlobalIlluminationTechniqueCapabilityRequirements::None;
        GlobalIlluminationTechniqueRequiredInput RequiredInputs =
            GlobalIlluminationTechniqueRequiredInput::None;
        GlobalIlluminationTechniqueOutputSemantics OutputSemantics;
        std::vector<GlobalIlluminationDebugView> SupportedDebugViews;
        std::filesystem::path ShaderArtifactCacheRoot;
        // Optional provider-owned pass used only for aggregate GPU timing.
        // Runtime never needs to know the concrete implementation pass name.
        std::string GpuTimingPassName;
        OfflineViewportRendererSupportQuery QuerySupport;
        OfflineViewportRendererFactory Factory;
    };

    namespace Detail
    {
        struct OfflineViewportRendererRegistryState;
    }

    class OfflineViewportRendererProviderRegistry;

    class OfflineViewportRendererProviderRegistration final
    {
    public:
        OfflineViewportRendererProviderRegistration() noexcept = default;
        ~OfflineViewportRendererProviderRegistration();

        OfflineViewportRendererProviderRegistration(
            const OfflineViewportRendererProviderRegistration&) = delete;
        OfflineViewportRendererProviderRegistration& operator=(
            const OfflineViewportRendererProviderRegistration&) = delete;
        OfflineViewportRendererProviderRegistration(
            OfflineViewportRendererProviderRegistration&& other) noexcept;
        OfflineViewportRendererProviderRegistration& operator=(
            OfflineViewportRendererProviderRegistration&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        void Release() noexcept;

    private:
        friend class OfflineViewportRendererProviderRegistry;

        OfflineViewportRendererProviderRegistration(
            std::shared_ptr<Detail::OfflineViewportRendererRegistryState> state,
            std::string id) noexcept;

        std::shared_ptr<Detail::OfflineViewportRendererRegistryState> mState;
        std::string mId;
    };

    class OfflineViewportRendererSessionLease final
    {
    public:
        OfflineViewportRendererSessionLease() noexcept = default;
        ~OfflineViewportRendererSessionLease();

        OfflineViewportRendererSessionLease(
            const OfflineViewportRendererSessionLease&) = delete;
        OfflineViewportRendererSessionLease& operator=(
            const OfflineViewportRendererSessionLease&) = delete;
        OfflineViewportRendererSessionLease(
            OfflineViewportRendererSessionLease&& other) noexcept;
        OfflineViewportRendererSessionLease& operator=(
            OfflineViewportRendererSessionLease&& other) noexcept;

        [[nodiscard]] IOfflineViewportRendererSession* Get() const noexcept;
        [[nodiscard]] IOfflineViewportRendererSession* operator->() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;
        void Release() noexcept;

    private:
        friend class OfflineViewportRendererProviderRegistry;

        OfflineViewportRendererSessionLease(
            Scope<IOfflineViewportRendererSession> session,
            std::shared_ptr<Detail::OfflineViewportRendererRegistryState> state,
            std::string id) noexcept;

        Scope<IOfflineViewportRendererSession> mSession;
        std::shared_ptr<Detail::OfflineViewportRendererRegistryState> mState;
        std::string mId;
    };

    class OfflineViewportRendererProviderRegistry final
    {
    public:
        [[nodiscard]] static OfflineViewportRendererProviderRegistry& Get();

        OfflineViewportRendererProviderRegistry(
            const OfflineViewportRendererProviderRegistry&) = delete;
        OfflineViewportRendererProviderRegistry& operator=(
            const OfflineViewportRendererProviderRegistry&) = delete;

        [[nodiscard]] OfflineViewportRendererProviderRegistration Register(
            OfflineViewportRendererProviderDescriptor descriptor);
        void Unregister(std::string_view id);
        [[nodiscard]] bool Contains(std::string_view id) const;
        [[nodiscard]] std::vector<OfflineViewportRendererProviderDescriptor>
            Enumerate() const;
        [[nodiscard]] OfflineViewportRendererSupport QuerySupport(
            std::string_view id,
            const RenderCapabilities& capabilities) const;
        [[nodiscard]] OfflineViewportRendererSessionLease Create(
            std::string_view id,
            const OfflineViewportRendererCreateContext& context);
        [[nodiscard]] size_t GetActiveSessionCount(std::string_view id) const;

    private:
        OfflineViewportRendererProviderRegistry();

        std::shared_ptr<Detail::OfflineViewportRendererRegistryState> mState;
    };
}
