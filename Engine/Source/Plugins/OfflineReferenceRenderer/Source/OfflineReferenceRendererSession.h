#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RayTracing/RayTracingScene.h"
#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>

namespace HEngine
{
    class ShaderManager;

    struct ReferencePathTracerDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        RayTracingScene* Scene = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        std::filesystem::path OfflineSettingsPath;
        RenderBackend Backend = RenderBackend::DX12;
        uint32_t FramesInFlight = 1;
        bool bOfflineViewportSession = false;
    };

    struct ReferencePathTracerFrameParameters
    {
        RenderCamera Camera;
        GlobalIlluminationHistorySignature HistorySignature;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FixedSeed = 0x48454e47u;
        float MaximumRayDistance = 1000.0f;
        glm::vec3 EnvironmentRadiance{ 0.0f };
        bool bSampleSceneEnvironment = false;
        // Fingerprint of scene state that changes the reference integral but
        // is intentionally ignored by realtime temporal reprojection (for
        // example ordinary instance transforms).
        uint64_t SceneStateFingerprint = 0;
        uint64_t CompletedSubmission = 0;
    };

    [[nodiscard]] uint64_t ComputeReferencePathTracerAccumulationFingerprint(
        const ReferencePathTracerFrameParameters& frame,
        const GlobalIlluminationSettings& settings) noexcept;

    struct ReferencePathTracerOutputs
    {
        TextureHandle AccumulatedRadiance;
        TextureHandle DirectDiffuse;
        TextureHandle IndirectDiffuse;
        TextureHandle Albedo;
        TextureHandle WorldNormal;
        TextureHandle ViewDepth;
        TextureHandle MaterialId;
        TextureHandle InstanceId;
        TextureHandle SampleCount;
    };

    struct ReferencePathTracerNativeOutputs
    {
        nvrhi::TextureHandle AccumulatedRadiance;
        nvrhi::TextureHandle DirectDiffuse;
        nvrhi::TextureHandle IndirectDiffuse;
        nvrhi::TextureHandle Albedo;
        nvrhi::TextureHandle WorldNormal;
        nvrhi::TextureHandle ViewDepth;
        nvrhi::TextureHandle MaterialId;
        nvrhi::TextureHandle InstanceId;
        nvrhi::TextureHandle SampleCount;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint64_t Generation = 0;
    };

    namespace Detail
    {
        enum class ReferenceOfflineControlCommand : uint8_t
        {
            Pause,
            Resume,
            Restart,
            Cancel,
            SamplesSubmitted,
        };

        [[nodiscard]] uint32_t ResolveReferenceOfflineDispatchCount(
            const OfflineViewportRendererSettings& settings,
            OfflineViewportSessionState state,
            uint64_t accumulatedSamples,
            bool bRestartRequired,
            bool bFrozen) noexcept;
        [[nodiscard]] OfflineViewportSessionState
            ResolveReferenceOfflineControlState(
                OfflineViewportSessionState state,
                ReferenceOfflineControlCommand command,
                uint64_t accumulatedSamples,
                uint64_t targetSamples,
                bool bRestartRequired) noexcept;
        void ValidateReferenceOfflineExportRequest(
            OfflineViewportAov aov,
            OfflineViewportOutputFormat format);
    }

    class ReferencePathTracer final : public IOfflineViewportRendererSession
    {
    public:
        explicit ReferencePathTracer(const ReferencePathTracerDesc& desc);
        ~ReferencePathTracer() override;

        ReferencePathTracer(const ReferencePathTracer&) = delete;
        ReferencePathTracer& operator=(const ReferencePathTracer&) = delete;

        [[nodiscard]] std::string_view GetProviderId() const noexcept override;
        [[nodiscard]] const OfflineViewportRendererSettings&
            GetOfflineSettings() const noexcept override;
        void ApplyOfflineSettings(
            const OfflineViewportRendererSettings& settings) override;
        [[nodiscard]] OfflineViewportRendererProgress
            GetOfflineProgress() const override;
        void SetLastGpuMilliseconds(double milliseconds) noexcept override;
        [[nodiscard]] std::span<const OfflineViewportAov>
            GetAvailableAovs() const noexcept override;
        [[nodiscard]] OfflineViewportNativeAov GetNativeAov(
            OfflineViewportAov aov) const override;

        void Pause() noexcept override;
        void Resume() noexcept override;
        void Restart() override;
        void Cancel() noexcept override;

        [[nodiscard]] TextureReadbackHandle RequestExport(
            OfflineViewportAov aov,
            std::filesystem::path outputPath,
            OfflineViewportOutputFormat format) override;
        [[nodiscard]] bool IsExportValid(
            TextureReadbackHandle handle) const noexcept override;
        [[nodiscard]] TextureReadbackResult GetExportResult(
            TextureReadbackHandle handle) const override;
        void ReleaseExport(TextureReadbackHandle handle) override;

        void PrepareFrame(
            const ReferencePathTracerFrameParameters& frame,
            const GlobalIlluminationSettings& settings);

        [[nodiscard]] bool IsSupported(
            const RenderCapabilities& capabilities) const override;
        [[nodiscard]] GlobalIlluminationPrepareResult PrepareFrame(
            const GlobalIlluminationFrameContext& frame,
            const GlobalIlluminationSettings& settings) override;
        [[nodiscard]] GlobalIlluminationOutputs AddPasses(
            RenderGraph& renderGraph,
            const GlobalIlluminationInputs& inputs,
            const GlobalIlluminationSettings& settings) override;
        void ResetHistory(GlobalIlluminationHistoryResetReason reason) override;
        [[nodiscard]] bool RequiresContinuousUpdate() const noexcept override;
        [[nodiscard]] GlobalIlluminationStats GetStats() const override;

        [[nodiscard]] const ReferencePathTracerOutputs& GetOutputs() const noexcept;
        [[nodiscard]] const ReferencePathTracerNativeOutputs&
            GetNativeOutputs() const noexcept;
        [[nodiscard]] bool ConsumeGraphRebuildRequest() noexcept;

        void UpdateImportedResources(RenderGraph& renderGraph) override;
        void NotifyFrameSubmitted(uint64_t submission) noexcept override;
        void AbortFrame() noexcept override;
        void Collect(uint64_t completedSubmission) override;
        void Clear(uint64_t completedSubmission) override;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
