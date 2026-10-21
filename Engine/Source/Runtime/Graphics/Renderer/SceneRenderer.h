#pragma once

#include "Runtime/Graphics/Renderer/RenderPipeline.h"

namespace HEngine
{
    class SceneRenderer final : public RenderPipeline
    {
    public:
        explicit SceneRenderer(const SceneRendererDesc& desc);
        ~SceneRenderer();

        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        void RecordFrame(const RenderWorld& world, const SceneRendererFrameDesc& frame);
        [[nodiscard]] Json GetRecordedRasterCaptureMetadata() const override;
        [[nodiscard]] bool IsCaptureSourceAvailable(FrameCaptureSource source) const noexcept override;

        // Must be called outside RecordFrame. A successful return means all
        // required GPU ShaderHandles exist and the program is now current.
        [[nodiscard]] MaterialShaderRefreshResult RefreshMaterialShaderAtFrameBoundary(
            std::string_view sourcePath);
        // RuntimeRenderer calls this for preview renderers after the shared
        // manager publishes a new generation and the device is idle.
        void ClearMaterialShaderPipelineCache();

        // Development visualization for cascade coverage and blend regions.
        // Disabled by default and does not alter the shadow resource layout.
        void SetDirectionalShadowCascadeDebug(bool bEnabled) noexcept;

        // Development-only PBR component visualization. Disabled by default;
        // all modes keep the normal HDR/tone-map output path intact.
        void SetPbrDebugMode(ScenePbrDebugMode mode) noexcept;

        // Configure outside RecordFrame. RuntimeRenderer waits for the device
        // before changing techniques so persistent histories can retire safely.
        void SetGlobalIlluminationSettings(
            const GlobalIlluminationSettings& settings);
        void SetAntiAliasingSettings(const AntiAliasingSettings& settings);
        // All participating views prepare before any view commits. Only the
        // rendering configuration fields in target are consumed; device,
        // backend and resource managers remain owned by this renderer.
        [[nodiscard]] SceneRendererConfigurationPreparationResult PrepareRenderingConfiguration(
            const SceneRendererDesc& target, const RenderWorld& world,
            const SceneRendererFrameDesc* preparationFrame = nullptr);
        void CommitPreparedRenderingConfiguration() noexcept;
        void AbortPreparedRenderingConfiguration() noexcept;
        void PrepareRenderingPath(RenderingPath path);
        void SetPostProcessSettings(const ScenePostProcessSettings& settings);
        void SetUpscalingSharpness(float sharpness) override;
        void SetCloudSettings(const CloudSettings& settings);
        void SetAmbientOcclusionSettings(const SceneAmbientOcclusionSettings& settings);
        void SetReflectionSettings(const SceneReflectionSettings& settings);
        void SetGpuOcclusionEnabled(bool bEnabled);
        void SetGpuBatchingEnabled(bool bEnabled);
        void SetGpuShadowBatchingEnabled(bool bEnabled);
        void SetLodSettings(bool bEnabled, float maxPixelError);
        void PrepareReflectionProbeResources(const RenderWorld& world);
        void CommitRenderingPath(RenderingPath path);
        [[nodiscard]] RenderingPath GetRenderingPath() const noexcept;
        void SetRenderSettings(
            const AntiAliasingSettings& antiAliasing,
            const GlobalIlluminationSettings& globalIllumination);
        [[nodiscard]] const AntiAliasingSettings&
            GetAntiAliasingSettings() const noexcept;
        [[nodiscard]] const AntiAliasingSettings&
            GetRequestedAntiAliasingSettings() const noexcept;
        [[nodiscard]] const AntiAliasingSettings&
            GetEffectiveAntiAliasingSettings() const noexcept;
        [[nodiscard]] const AntiAliasingCapabilities&
            GetAntiAliasingCapabilities() const noexcept;
        [[nodiscard]] AntiAliasingSelection
            GetAntiAliasingSelection() const;
        void RestartAntiAliasingConvergence() noexcept;
        // Call between frames; invalidates CPU history state without replacing GPU resources.
        void ResetTemporalHistory();
        void ResetGlobalIlluminationHistory(
            GlobalIlluminationHistoryResetReason reason =
                GlobalIlluminationHistoryResetReason::Explicit);
        [[nodiscard]] const GlobalIlluminationSettings&
            GetGlobalIlluminationSettings() const noexcept;
        [[nodiscard]] const GlobalIlluminationSettings&
            GetRequestedGlobalIlluminationSettings() const noexcept;
        [[nodiscard]] const GlobalIlluminationSettings&
            GetEffectiveGlobalIlluminationSettings() const noexcept;
        [[nodiscard]] GlobalIlluminationSelection
            GetGlobalIlluminationSelection() const;
        [[nodiscard]] std::vector<GlobalIlluminationTechniqueDescriptor>
            EnumerateGlobalIlluminationTechniques() const;
        [[nodiscard]] bool RequiresContinuousUpdate() const noexcept;
        [[nodiscard]] nvrhi::TextureHandle GetOfflineViewportBeautyTexture() const;
        void ActivateOfflineViewportRenderer(std::string providerId);
        void DeactivateOfflineViewportRenderer();
        [[nodiscard]] std::string GetActiveOfflineViewportRendererId() const;
        [[nodiscard]] IOfflineViewportRendererSession*
            GetOfflineViewportRendererSession() const noexcept;
        [[nodiscard]] std::vector<OfflineViewportRendererProviderDescriptor>
            EnumerateOfflineViewportRenderers() const;

        // Synchronously decodes and precomputes the IBL cache on NVRHI. Call
        // outside an active frame (normally during Game startup). Empty paths
        // represent an intentionally unlit environment and are immediately Ready.
        [[nodiscard]] SceneEnvironmentPreparation PrepareEnvironment(
            std::string_view path);
        [[nodiscard]] SceneEnvironmentPreparation GetEnvironmentPreparation(
            std::string_view path) const;
        [[nodiscard]] bool IsEnvironmentReady(std::string_view path) const;

        // Shared typed texture ownership for runtime/editor UI. Load outside an
        // active RecordFrame; descriptors are collected with frame submission.
        [[nodiscard]] ManagedTextureHandle LoadTexture(const TextureLoadDesc& desc);
        [[nodiscard]] const ManagedTexture& GetTexture(ManagedTextureHandle handle) const;
        void ReleaseTexture(
            ManagedTextureHandle handle,
            uint64_t retireAfterSubmission);
        size_t InvalidateFailedTextureLoads(std::string_view assetPath = {});
        size_t RequestTextureReload(std::string_view assetPath) override;
#if defined(HE_DEV_AGENT_RUNTIME)
        bool InjectUpscalingFailureForValidation(bool duringDispatch) override;
#endif

        // Configure outside an active frame. SwapChainAndOffscreen copies the
        // tone-mapped LDR scene after the runtime RmlUi composite and before
        // the window ImGui overlay; OffscreenOnly renders a dedicated preview
        // without UI/capture/present. A dedicated preview SceneRenderer can
        // record into the same open command list as the main renderer and share
        // the eventual RenderDevice submission.
        void ConfigureOffscreenOutput(
            uint32_t width,
            uint32_t height,
            std::string debugName = "SceneRenderer Offscreen Output",
            nvrhi::Format colorFormat = nvrhi::Format::RGBA8_UNORM);
        void DisableOffscreenOutput();
        [[nodiscard]] SceneRendererOffscreenOutput GetOffscreenOutput() const;

        // Submission is known only after RenderDevice executes the command list.
        // This call closes the frame-slot ownership interval used by transient
        // RenderGraph resources.
        void NotifyFrameSubmitted(uint32_t frameSlot, uint64_t submission);
        void AbortFrame(uint32_t frameSlot) noexcept;

        // Call after RenderDevice::WaitForIdle and before resizing the swapchain;
        // this releases imported backbuffer/framebuffer references required by
        // DXGI ResizeBuffers.
        void Resize(uint32_t width, uint32_t height);
        void ClearCaches();
        void SetTransientCacheBudget(uint64_t bytes) noexcept override;
        void SetTextureStreamingBudget(uint64_t bytes) noexcept override;

        // Maps the per-frame indirect-argument readbacks. The caller must first
        // wait for the graphics queue to become idle.
        void CollectGpuCullingStats();

        // Resolves all submitted timer queries. The caller must first wait for
        // the graphics queue to become idle.
        void CollectGpuTimingStats();

        [[nodiscard]] const SceneRendererStats& GetStats() const noexcept;
        [[nodiscard]] RenderGraphDiagnosticSnapshot
            GetRenderGraphDiagnosticSnapshot() const;
        [[nodiscard]] std::string BuildRenderGraphDebugDump() const;
        [[nodiscard]] std::string BuildGpuTimingDebugDump() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
