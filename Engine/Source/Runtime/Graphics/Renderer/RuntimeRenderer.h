#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"
#include "Runtime/Graphics/RHI/TextureManager.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"
#include "Runtime/Graphics/Renderer/RuntimeUiRenderGraphPass.h"
#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    class RenderSurface;
    struct MaterialShaderRefreshResult;
    struct RenderWorld;
    struct ResolvedRenderSettings;
    struct RenderSettingsChangePlan;
    struct RenderGraphDiagnosticSnapshot;

    struct RuntimeRendererDesc
    {
        RenderSurface* Surface = nullptr;
		RenderBackend Backend = RenderBackend::DX12;
        RenderAdapterSelectionOptions AdapterSelection;
        std::filesystem::path ShaderCacheDirectory;
        std::filesystem::path AssetRootDirectory;
        std::filesystem::path EngineAssetRootDirectory;
        bool bEnableValidation = false;
        bool bEnableGpuTiming = false;
        GlobalIlluminationSettings GlobalIllumination;
        AntiAliasingSettings AntiAliasing;
        std::uint32_t PbrDebugMode = 0;
        bool bShadowCascadeDebug = false;
    };

    enum class RuntimeEnvironmentState : uint8_t
    {
        Unrequested,
        Preparing,
        Ready,
        Failed,
    };

    struct RuntimeEnvironmentPreparation
    {
        RuntimeEnvironmentState State = RuntimeEnvironmentState::Unrequested;
        std::string Path;
        std::string Error;
    };

    struct RuntimeOverlayContext
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::IFramebuffer* Framebuffer = nullptr;
        nvrhi::ITexture* ColorTarget = nullptr;
        nvrhi::ITexture* StencilTarget = nullptr;
        nvrhi::ITexture* BackdropTexture = nullptr;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSlot = 0;
        uint64_t CompletedSubmission = 0;
    };

    // Called synchronously by SceneRenderer's graph-owned UI pass. Implementations
    // may record NVRHI commands, but never retain the frame-owned pointers.
    class IRuntimeOverlayRecorder
    {
    public:
        virtual ~IRuntimeOverlayRecorder() = default;
        [[nodiscard]] virtual bool HasDynamicSceneOverlay() const noexcept { return false; }
        [[nodiscard]] virtual RuntimeUiRenderMode GetSceneOverlayMode() const noexcept
        {
            return RuntimeUiRenderMode::Direct;
        }
        [[nodiscard]] virtual RuntimeUiTransitionParameters GetSceneOverlayTransition() const noexcept
        {
            return {};
        }
        virtual void RecordSceneOverlay(const RuntimeOverlayContext& context) = 0;
        virtual void NotifySceneOverlayFrameSubmitted(
            uint32_t frameSlot,
            uint64_t submission)
        {
            (void)frameSlot;
            (void)submission;
        }
        virtual void AbortSceneOverlayFrame(uint32_t frameSlot) noexcept
        {
            (void)frameSlot;
        }
        virtual void RecordWindowOverlay(const RuntimeOverlayContext& context) = 0;
    };

    struct RuntimeRendererFrameDesc
    {
        Ref<const RenderWorld> World;
        IRuntimeOverlayRecorder* OverlayRecorder = nullptr;
        std::filesystem::path CapturePath;
    };

    struct RuntimeRendererFrameResult
    {
        uint32_t FrameSlot = 0;
        uint64_t Submission = 0;
        TextureReadbackHandle Capture;
    };

	struct RuntimeOffscreenViewHandle
	{
		uint32_t Index = std::numeric_limits<uint32_t>::max();
		uint32_t Generation = 0;

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return Index != std::numeric_limits<uint32_t>::max() && Generation != 0;
		}

		[[nodiscard]] bool operator==(const RuntimeOffscreenViewHandle&) const = default;
	};

	struct RuntimeOffscreenOutput
	{
		nvrhi::TextureHandle Color;
		nvrhi::TextureHandle Depth;
		uint32_t Width = 0;
		uint32_t Height = 0;
		nvrhi::Format ColorFormat = nvrhi::Format::UNKNOWN;
		nvrhi::Format DepthFormat = nvrhi::Format::UNKNOWN;
		uint64_t Generation = 0;

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return Color && Depth && Width > 0 && Height > 0 && Generation > 0;
		}
	};

    // Owns the runtime RenderDevice, SceneRenderer and frame submission. This is
    // the only Application-facing owner of a graphics command list.
    class RuntimeRenderer final
    {
    public:
        RuntimeRenderer();
        ~RuntimeRenderer();

        RuntimeRenderer(const RuntimeRenderer&) = delete;
        RuntimeRenderer& operator=(const RuntimeRenderer&) = delete;

        void Initialize(const RuntimeRendererDesc& desc);
        void Shutdown();

        [[nodiscard]] RuntimeRendererFrameResult RenderFrame(
            const RuntimeRendererFrameDesc& desc,
            bool bVSync);
        [[nodiscard]] MaterialShaderRefreshResult RefreshMaterialShaderAtFrameBoundary(
            std::string_view sourcePath);
        void Resize(uint32_t width, uint32_t height);
        void WaitForIdle();
        void SetAntiAliasingSettings(const AntiAliasingSettings& settings);
        void ApplyRenderSettingsBatch(
            const ResolvedRenderSettings& settings,
            const RenderSettingsChangePlan& plan);
        [[nodiscard]] AntiAliasingSettings GetAntiAliasingSettings() const;
        [[nodiscard]] AntiAliasingSettings
            GetRequestedAntiAliasingSettings() const;
        [[nodiscard]] AntiAliasingSettings
            GetEffectiveAntiAliasingSettings() const;
        [[nodiscard]] AntiAliasingCapabilities
            GetAntiAliasingCapabilities() const;
        [[nodiscard]] AntiAliasingSelection GetAntiAliasingSelection() const;
        void SetGlobalIlluminationSettings(
            const GlobalIlluminationSettings& settings);
        void ResetGlobalIlluminationHistory();
        [[nodiscard]] GlobalIlluminationSettings
            GetGlobalIlluminationSettings() const;
        [[nodiscard]] GlobalIlluminationSettings
            GetRequestedGlobalIlluminationSettings() const;
        [[nodiscard]] GlobalIlluminationSettings
            GetEffectiveGlobalIlluminationSettings() const;
        [[nodiscard]] GlobalIlluminationSelection
            GetGlobalIlluminationSelection() const;
        [[nodiscard]] std::vector<GlobalIlluminationTechniqueDescriptor>
            EnumerateGlobalIlluminationTechniques() const;
        [[nodiscard]] RenderCapabilities GetCapabilities() const;
        void ActivateOfflineViewportRenderer(std::string providerId);
        void DeactivateOfflineViewportRenderer();
        [[nodiscard]] std::string GetActiveOfflineViewportRendererId() const;
        [[nodiscard]] IOfflineViewportRendererSession*
            GetOfflineViewportRendererSession() const noexcept;
        [[nodiscard]] std::vector<OfflineViewportRendererProviderDescriptor>
            EnumerateOfflineViewportRenderers() const;

        [[nodiscard]] RuntimeEnvironmentPreparation PrepareEnvironment(
            std::string_view path);
        [[nodiscard]] RuntimeEnvironmentPreparation GetEnvironmentPreparation(
            std::string_view path) const;
        [[nodiscard]] bool IsEnvironmentReady(std::string_view path) const;

		[[nodiscard]] ManagedTextureHandle LoadTexture(const TextureLoadDesc& desc);
		[[nodiscard]] const ManagedTexture& GetTexture(ManagedTextureHandle handle) const;
		void ReleaseTexture(ManagedTextureHandle handle, uint64_t retireSubmission);

		void EnableMainOffscreenOutput();
		[[nodiscard]] RuntimeOffscreenOutput GetMainOffscreenOutput() const;
		[[nodiscard]] RuntimeOffscreenViewHandle CreateOffscreenView(
			uint32_t width,
			uint32_t height,
			std::string debugName);
		void ResizeOffscreenView(
			RuntimeOffscreenViewHandle handle,
			uint32_t width,
			uint32_t height);
		void SubmitOffscreenView(
			RuntimeOffscreenViewHandle handle,
			Ref<const RenderWorld> world);
		[[nodiscard]] RuntimeOffscreenOutput GetOffscreenOutput(
			RuntimeOffscreenViewHandle handle) const;
		[[nodiscard]] TextureReadbackHandle RequestOffscreenCapture(
			RuntimeOffscreenViewHandle handle,
			std::filesystem::path outputPath);
		void DestroyOffscreenView(RuntimeOffscreenViewHandle handle);

        void CollectCaptures(uint64_t completedSubmission);
        [[nodiscard]] bool IsCaptureValid(TextureReadbackHandle handle) const noexcept;
        [[nodiscard]] TextureReadbackResult GetCaptureResult(
            TextureReadbackHandle handle) const;
        void ReleaseCapture(TextureReadbackHandle handle);

        [[nodiscard]] bool IsInitialized() const noexcept;
        [[nodiscard]] nvrhi::IDevice* GetDevice() const;
        [[nodiscard]] uint32_t GetFramesInFlight() const;
        [[nodiscard]] uint32_t GetWidth() const;
        [[nodiscard]] uint32_t GetHeight() const;
		[[nodiscard]] RenderBackend GetBackend() const;
		[[nodiscard]] std::string BuildSceneStatsSummary() const;
		[[nodiscard]] RenderGraphDiagnosticSnapshot
			GetRenderGraphDiagnosticSnapshot() const;
		[[nodiscard]] std::string BuildRenderGraphDebugDump() const;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
