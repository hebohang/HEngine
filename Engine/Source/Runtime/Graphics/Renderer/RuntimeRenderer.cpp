#include "Hepch.h"

#include "Runtime/Graphics/Renderer/RuntimeRenderer.h"

#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/Graphics/RHI/RenderSurface.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/Renderer/SceneRenderer.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"
#include "Runtime/Graphics/Settings/RenderSettings.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] RuntimeEnvironmentPreparation ToRuntimeEnvironmentPreparation(
            const SceneEnvironmentPreparation& preparation)
        {
            RuntimeEnvironmentState state = RuntimeEnvironmentState::Unrequested;
            switch (preparation.State)
            {
                case SceneEnvironmentState::Unrequested:
                    state = RuntimeEnvironmentState::Unrequested;
                    break;
                case SceneEnvironmentState::Preparing:
                    state = RuntimeEnvironmentState::Preparing;
                    break;
                case SceneEnvironmentState::Ready:
                    state = RuntimeEnvironmentState::Ready;
                    break;
                case SceneEnvironmentState::Failed:
                    state = RuntimeEnvironmentState::Failed;
                    break;
            }
            return {
                .State = state,
                .Path = preparation.Path,
                .Error = preparation.Error,
            };
        }

		[[nodiscard]] RuntimeOffscreenOutput ToRuntimeOffscreenOutput(
			const SceneRendererOffscreenOutput& output)
		{
			return {
				.Color = output.Color,
				.Depth = output.Depth,
				.Width = output.Width,
				.Height = output.Height,
				.ColorFormat = output.ColorFormat,
				.DepthFormat = output.DepthFormat,
				.Generation = output.Generation,
			};
		}

    }

    struct RuntimeRenderer::Impl
    {
		struct OffscreenView
		{
			Scope<SceneRenderer> Renderer;
			Ref<const RenderWorld> World;
			std::string DebugName;
			TextureReadbackHandle PendingCapture;
			uint32_t Generation = 1;
			bool bLive = false;
			bool bSubmitted = false;
		};

        RenderDevice Device;
        Scope<ShaderManager> MaterialShaderHandles;
        Scope<MaterialShaderManager> MaterialShaders;
        Scope<SceneRenderer> Scene;
        Scope<TextureReadbackQueue> Readbacks;
		SceneRendererDesc SceneDesc;
		std::vector<OffscreenView> OffscreenViews;
		std::vector<uint32_t> FreeOffscreenViews;
        RenderWorld EmptyWorld;
        TextureReadbackHandle RecordingCapture;
        uint64_t LastSubmission = 0;
		Ref<const RenderWorld> LastMainWorld;
		bool bMainOffscreenContentValid = false;
        bool bEffekseerInitialized = false;
		bool bMainOffscreenEnabled = false;
    };

    RuntimeRenderer::RuntimeRenderer() = default;

    RuntimeRenderer::~RuntimeRenderer()
    {
        try
        {
            Shutdown();
        }
        catch (...)
        {
            // Destruction must remain noexcept. Explicit Shutdown still reports
            // device-idle and resource-retirement failures to Application.
        }
    }

    void RuntimeRenderer::Initialize(const RuntimeRendererDesc& desc)
    {
        if (mImpl)
            throw std::logic_error("RuntimeRenderer is already initialized");
        if (!desc.Surface)
            throw std::invalid_argument("RuntimeRenderer requires a RenderSurface");
        if (desc.ShaderCacheDirectory.empty())
            throw std::invalid_argument("RuntimeRenderer requires a DXIL shader cache directory");

        Scope<Impl> impl = CreateScope<Impl>();
        try
        {
            impl->Device.Initialize(
                desc.Backend,
                *desc.Surface,
                desc.bEnableValidation,
                desc.AdapterSelection);
            const uint32_t framesInFlight = impl->Device.GetCapabilities().FramesInFlight;
            EffekseerRuntime::Initialize({
                .Device = impl->Device.GetDevice(),
                .ColorFormat = nvrhi::Format::RGBA16_FLOAT,
                .DepthFormat = nvrhi::Format::D32,
                .FramesInFlight = framesInFlight,
                .bReversedDepth = false,
            });
            impl->bEffekseerInitialized = true;
            impl->MaterialShaderHandles =
                CreateScope<ShaderManager>(impl->Device.GetDevice());
            impl->MaterialShaders = CreateScope<MaterialShaderManager>(
                impl->MaterialShaderHandles.get(),
                desc.ShaderCacheDirectory.parent_path());
            impl->SceneDesc = SceneRendererDesc{
                .Device = impl->Device.GetDevice(),
                .FramesInFlight = framesInFlight,
                .ShaderCacheDirectory = desc.ShaderCacheDirectory,
                .AssetRootDirectory = desc.AssetRootDirectory,
                .EngineAssetRootDirectory = desc.EngineAssetRootDirectory,
                .SharedMaterialShaderManager = impl->MaterialShaders.get(),
                .SharedMaterialShaderHandleManager = impl->MaterialShaderHandles.get(),
                .Backend = desc.Backend,
                .Capabilities = impl->Device.GetCapabilities(),
                .GlobalIllumination = desc.GlobalIllumination,
                .AntiAliasing = desc.AntiAliasing,
                .bHeapDirectlyIndexed =
                    impl->Device.GetCapabilities().HeapDirectlyIndexed.bEnabled,
                .bEnableGpuTiming = desc.bEnableGpuTiming,
            };
            impl->Scene = CreateScope<SceneRenderer>(impl->SceneDesc);
            const ScenePbrDebugMode pbrDebugMode =
                static_cast<ScenePbrDebugMode>(desc.PbrDebugMode);
            const bool bCascadeDebug = desc.bShadowCascadeDebug;
            impl->Scene->SetPbrDebugMode(pbrDebugMode);
            impl->Scene->SetDirectionalShadowCascadeDebug(bCascadeDebug);
            if (pbrDebugMode != ScenePbrDebugMode::Disabled || bCascadeDebug)
            {
                HE_CORE_INFO(
                    "Development render debug enabled: pbrMode={} cascades={}",
                    static_cast<uint32_t>(pbrDebugMode),
                    bCascadeDebug ? 1 : 0);
            }
            impl->Readbacks = CreateScope<TextureReadbackQueue>(impl->Device.GetDevice());
        }
        catch (...)
        {
            impl->Scene.reset();
            impl->Readbacks.reset();
            impl->MaterialShaders.reset();
            impl->MaterialShaderHandles.reset();
            if (impl->bEffekseerInitialized)
            {
                EffekseerRuntime::Shutdown();
                impl->bEffekseerInitialized = false;
            }
            impl->Device.Shutdown();
            throw;
        }
        mImpl = std::move(impl);
    }

    void RuntimeRenderer::Shutdown()
    {
        if (!mImpl)
            return;

        if (mImpl->Device.IsInitialized())
            mImpl->Device.WaitForIdle();
        if (mImpl->Readbacks)
        {
            mImpl->Readbacks->Drain(mImpl->LastSubmission);
            mImpl->Readbacks->Clear();
            mImpl->Readbacks.reset();
        }
		mImpl->OffscreenViews.clear();
		mImpl->FreeOffscreenViews.clear();
        mImpl->Scene.reset();
        mImpl->MaterialShaders.reset();
        mImpl->MaterialShaderHandles.reset();
        if (mImpl->bEffekseerInitialized)
        {
            EffekseerRuntime::Shutdown();
            mImpl->bEffekseerInitialized = false;
        }
        mImpl->Device.Shutdown();
        mImpl.reset();
    }

    MaterialShaderRefreshResult RuntimeRenderer::RefreshMaterialShaderAtFrameBoundary(
        std::string_view sourcePath)
    {
        if (!IsInitialized())
            throw std::logic_error(
                "RuntimeRenderer material shader refresh requires Initialize");
        if (sourcePath.empty())
            throw std::invalid_argument("Material shader source path is empty");
        MaterialShaderRefreshResult result =
            mImpl->Scene->RefreshMaterialShaderAtFrameBoundary(sourcePath);
        if (result.bPublished && result.Program && result.Program->GetGeneration() > 1)
        {
            for (Impl::OffscreenView& view : mImpl->OffscreenViews)
            {
                if (view.bLive && view.Renderer)
                    view.Renderer->ClearMaterialShaderPipelineCache();
            }
        }
        return result;
    }

    RuntimeRendererFrameResult RuntimeRenderer::RenderFrame(
        const RuntimeRendererFrameDesc& desc,
        bool bVSync)
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer RenderFrame called before Initialize");
        if (mImpl->RecordingCapture)
            throw std::logic_error("RuntimeRenderer retained an unsubmitted capture from a prior frame");
		if (desc.World && !desc.World->Environment.HdrTexturePath.empty())
		{
			const SceneEnvironmentPreparation preparation =
				mImpl->Scene->GetEnvironmentPreparation(
					desc.World->Environment.HdrTexturePath);
			if (preparation.State == SceneEnvironmentState::Unrequested)
			{
				const SceneEnvironmentPreparation result =
					mImpl->Scene->PrepareEnvironment(
						desc.World->Environment.HdrTexturePath);
				if (result.State == SceneEnvironmentState::Failed)
				{
					HE_CORE_ERROR(
						"Failed to prepare scene environment '{}': {}",
						result.Path,
						result.Error);
				}
			}
		}

        FrameContext& frame = mImpl->Device.BeginFrame();
        SwapChain& swapChain = mImpl->Device.GetSwapChain();
        IRuntimeOverlayRecorder* const overlayRecorder = desc.OverlayRecorder;
        const uint64_t completedSubmission = frame.CompletedSubmissionInstance;
        const std::filesystem::path capturePath = desc.CapturePath;
		bool bCaptureSubmitted = false;
		std::vector<uint32_t> recordedOffscreenViews;
		std::vector<TextureReadbackHandle> recordedOffscreenCaptures;
		size_t notifiedOffscreenViewCount = 0;
		size_t notifiedOffscreenCaptureCount = 0;
		bool bMainSceneRecorded = false;
		bool bMainSceneNotified = false;
		bool bSceneOverlayRecorded = false;
		bool bSceneOverlayNotified = false;
		if (desc.World && desc.World->bEditorView &&
			mImpl->LastMainWorld.get() != desc.World.get())
		{
			mImpl->Scene->RestartAntiAliasingConvergence();
		}
		const bool bReuseMainScene = mImpl->bMainOffscreenEnabled &&
			mImpl->bMainOffscreenContentValid && desc.World &&
			mImpl->LastMainWorld.get() == desc.World.get() &&
            !mImpl->Scene->RequiresContinuousUpdate() &&
            (!overlayRecorder || !overlayRecorder->HasDynamicSceneOverlay());
		auto publishSubmittedFrame = [&]()
		{
			if (!frame.bSubmissionIssued)
				return;

			mImpl->LastSubmission = frame.SubmissionInstance;
			if (mImpl->RecordingCapture && !bCaptureSubmitted)
			{
				mImpl->Readbacks->NotifySubmitted(
					mImpl->RecordingCapture,
					frame.SubmissionInstance);
				bCaptureSubmitted = true;
			}
			while (notifiedOffscreenCaptureCount < recordedOffscreenCaptures.size())
			{
				mImpl->Readbacks->NotifySubmitted(
					recordedOffscreenCaptures[notifiedOffscreenCaptureCount],
					frame.SubmissionInstance);
				++notifiedOffscreenCaptureCount;
			}
			if (bMainSceneRecorded && !bMainSceneNotified)
			{
				mImpl->Scene->NotifyFrameSubmitted(
					frame.FrameIndex,
					frame.SubmissionInstance);
				bMainSceneNotified = true;
			}
			if (bSceneOverlayRecorded && !bSceneOverlayNotified && overlayRecorder)
			{
				overlayRecorder->NotifySceneOverlayFrameSubmitted(
					frame.FrameIndex,
					frame.SubmissionInstance);
				bSceneOverlayNotified = true;
			}
			while (notifiedOffscreenViewCount < recordedOffscreenViews.size())
			{
				const uint32_t index =
					recordedOffscreenViews[notifiedOffscreenViewCount];
				mImpl->OffscreenViews[index].Renderer->NotifyFrameSubmitted(
					frame.FrameIndex,
					frame.SubmissionInstance);
				++notifiedOffscreenViewCount;
			}
		};
        try
        {
			for (uint32_t index = 0; index < mImpl->OffscreenViews.size(); ++index)
			{
				Impl::OffscreenView& view = mImpl->OffscreenViews[index];
				if (!view.bLive || !view.bSubmitted || !view.Renderer || !view.World)
					continue;
				const SceneRendererOffscreenOutput output =
					view.Renderer->GetOffscreenOutput();
				const TextureReadbackHandle offscreenCapture = view.PendingCapture;
				try
				{
					view.Renderer->RecordFrame(
						*view.World,
						SceneRendererFrameDesc{
							.CommandList = frame.GraphicsCommandList,
							.BackBufferFormat = output.ColorFormat,
							.Width = output.Width,
							.Height = output.Height,
							.FrameSlot = frame.FrameIndex,
							.CompletedSubmission = frame.CompletedSubmissionInstance,
							.OutputMode = SceneRendererOutputMode::OffscreenOnly,
							.CaptureRecorder = offscreenCapture
								? SceneRendererCaptureRecorder(
									[impl = mImpl.get(), offscreenCapture](
										const SceneRendererCaptureContext& context)
									{
										impl->Readbacks->RecordReserved(
											offscreenCapture,
											context.CommandList,
											context.BackBuffer);
									})
								: SceneRendererCaptureRecorder{},
						});
				}
				catch (...)
				{
					if (offscreenCapture && mImpl->Readbacks->IsValid(offscreenCapture))
						mImpl->Readbacks->CancelPending(offscreenCapture);
					view.PendingCapture = {};
					throw;
				}
				recordedOffscreenViews.push_back(index);
				if (offscreenCapture)
				{
					recordedOffscreenCaptures.push_back(offscreenCapture);
					view.PendingCapture = {};
				}
				view.bSubmitted = false;
			}
            mImpl->Scene->RecordFrame(
                desc.World ? *desc.World : mImpl->EmptyWorld,
                SceneRendererFrameDesc{
                    .CommandList = frame.GraphicsCommandList,
                    .BackBuffer = swapChain.GetCurrentBackBuffer(),
                    .BackBufferFormat = swapChain.GetFormat(),
                    .BackBufferInitialState =
                        swapChain.GetCurrentBackBufferInitialState(),
                    .Width = swapChain.GetWidth(),
                    .Height = swapChain.GetHeight(),
                    .FrameSlot = frame.FrameIndex,
                    .CompletedSubmission = frame.CompletedSubmissionInstance,
					.OutputMode = mImpl->bMainOffscreenEnabled
						? SceneRendererOutputMode::SwapChainAndOffscreen
						: SceneRendererOutputMode::SwapChain,
					.bReuseOffscreenScene = bReuseMainScene,
					.SceneOverlayMode = overlayRecorder
                        ? overlayRecorder->GetSceneOverlayMode()
                        : RuntimeUiRenderMode::Direct,
					.SceneOverlayTransition = overlayRecorder
                        ? overlayRecorder->GetSceneOverlayTransition()
                        : RuntimeUiTransitionParameters{},
					.SceneOverlayRecorder = overlayRecorder
                        ? SceneRendererOverlayRecorder(
							[overlayRecorder, completedSubmission, &bSceneOverlayRecorded](
                                const SceneRendererOverlayContext& context)
                            {
								bSceneOverlayRecorded = true;
                                overlayRecorder->RecordSceneOverlay(RuntimeOverlayContext{
                                    .CommandList = context.CommandList,
                                    .Framebuffer = context.Framebuffer,
                                    .ColorTarget = context.BackBuffer,
                                    .StencilTarget = context.StencilTarget,
                                    .BackdropTexture = context.BackdropTexture,
                                    .Width = context.Width,
                                    .Height = context.Height,
                                    .FrameSlot = context.FrameSlot,
                                    .CompletedSubmission = completedSubmission,
                                });
                            })
                        : SceneRendererOverlayRecorder{},
					.WindowOverlayRecorder = overlayRecorder
						? SceneRendererOverlayRecorder(
							[overlayRecorder, completedSubmission](
								const SceneRendererOverlayContext& context)
							{
								overlayRecorder->RecordWindowOverlay(RuntimeOverlayContext{
									.CommandList = context.CommandList,
									.Framebuffer = context.Framebuffer,
                                    .ColorTarget = context.BackBuffer,
									.Width = context.Width,
									.Height = context.Height,
									.FrameSlot = context.FrameSlot,
									.CompletedSubmission = completedSubmission,
								});
							})
						: SceneRendererOverlayRecorder{},
                    .CaptureRecorder = !capturePath.empty()
                        ? SceneRendererCaptureRecorder(
                            [impl = mImpl.get(), capturePath](
                                const SceneRendererCaptureContext& context)
                            {
                                impl->RecordingCapture = impl->Readbacks->Record(
                                    context.CommandList,
                                    context.BackBuffer,
                                    TextureReadbackDesc{ .OutputPath = capturePath });
                            })
                        : SceneRendererCaptureRecorder{},
                });
			bMainSceneRecorded = true;
            mImpl->Device.SubmitAndPresent(frame, bVSync);
            publishSubmittedFrame();
			if (!bReuseMainScene)
			{
				mImpl->LastMainWorld = desc.World;
				mImpl->bMainOffscreenContentValid = mImpl->bMainOffscreenEnabled &&
					static_cast<bool>(desc.World);
			}
            mImpl->Readbacks->Collect(frame.CompletedSubmissionInstance);
            RuntimeRendererFrameResult result{
                .FrameSlot = frame.FrameIndex,
                .Submission = frame.SubmissionInstance,
                .Capture = mImpl->RecordingCapture,
            };
            mImpl->RecordingCapture = {};
            return result;
        }
        catch (...)
        {
            const std::exception_ptr failure = std::current_exception();
			if (frame.bSubmissionIssued)
			{
				try
				{
					publishSubmittedFrame();
				}
				catch (const std::exception& notificationException)
				{
					HE_CORE_ERROR(
						"Failed to publish submitted renderer frame {0}: {1}",
						frame.SubmissionInstance,
						notificationException.what());
				}
			}
			else
			{
				mImpl->Device.AbortFrame(frame);
				if (bSceneOverlayRecorded && overlayRecorder)
					overlayRecorder->AbortSceneOverlayFrame(frame.FrameIndex);
				if (bMainSceneRecorded)
					mImpl->Scene->AbortFrame(frame.FrameIndex);
				for (const uint32_t index : recordedOffscreenViews)
					mImpl->OffscreenViews[index].Renderer->AbortFrame(frame.FrameIndex);
				for (const TextureReadbackHandle capture : recordedOffscreenCaptures)
				{
					if (mImpl->Readbacks->IsValid(capture))
						mImpl->Readbacks->CancelPending(capture);
				}
			}
            if (mImpl->RecordingCapture)
            {
                if (!frame.bSubmissionIssued && !bCaptureSubmitted)
                    mImpl->Readbacks->CancelPending(mImpl->RecordingCapture);
                mImpl->RecordingCapture = {};
            }
            std::rethrow_exception(failure);
        }
    }

    void RuntimeRenderer::Resize(uint32_t width, uint32_t height)
    {
        if (!IsInitialized() || width == 0 || height == 0)
            return;
        if (GetWidth() == width && GetHeight() == height)
            return;

        mImpl->Device.WaitForIdle();
		mImpl->bMainOffscreenContentValid = false;
        mImpl->Scene->Resize(width, height);
        mImpl->Device.Resize(width, height);
		if (mImpl->bMainOffscreenEnabled)
		{
			mImpl->Scene->ConfigureOffscreenOutput(
				width,
				height,
				"Runtime Main Scene Viewport");
		}
        mImpl->Readbacks->Collect(mImpl->LastSubmission);
    }

    void RuntimeRenderer::WaitForIdle()
    {
        if (!IsInitialized())
            return;
        mImpl->Device.WaitForIdle();
        mImpl->Scene->CollectGpuCullingStats();
        mImpl->Scene->CollectGpuTimingStats();
        mImpl->Readbacks->Drain(mImpl->LastSubmission);
    }

    void RuntimeRenderer::SetAntiAliasingSettings(
        const AntiAliasingSettings& settings)
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer AA configuration requires Initialize");
        mImpl->Device.WaitForIdle();
        mImpl->SceneDesc.AntiAliasing = settings;
        mImpl->Scene->SetAntiAliasingSettings(settings);
        for (Impl::OffscreenView& view : mImpl->OffscreenViews)
        {
            if (view.bLive && view.Renderer)
                view.Renderer->SetAntiAliasingSettings(settings);
        }
        mImpl->bMainOffscreenContentValid = false;
    }

    void RuntimeRenderer::ApplyRenderSettingsBatch(
        const ResolvedRenderSettings& settings,
        const RenderSettingsChangePlan& plan)
    {
        if (!IsInitialized())
            throw std::logic_error(
                "RuntimeRenderer settings transaction requires Initialize");

        bool bRendererSettingsChanged = false;
        bool bPbrDebugChanged = false;
        bool bShadowDebugChanged = false;
        for (const std::string& id : plan.ChangedPropertyIds)
        {
            const std::string_view propertyId(id);
            bRendererSettingsChanged |=
                propertyId.starts_with("Rendering.AntiAliasing.") ||
                propertyId.starts_with("Rendering.GI.") ||
                propertyId == RenderSettingIds::GiDebugView ||
                propertyId == RenderSettingIds::GiDirectOnly;
            bPbrDebugChanged |= propertyId == RenderSettingIds::PbrDebugMode;
            bShadowDebugChanged |=
                propertyId == RenderSettingIds::ShadowCascadeDebug;
        }

        AntiAliasingSettings antiAliasing = settings.Requested.AntiAliasing;
        antiAliasing.CaptureSupersampleScale =
            settings.Capture.SupersampleScale;
        GlobalIlluminationSettings globalIllumination =
            settings.Requested.GlobalIllumination;
        globalIllumination.DebugView = settings.Debug.GiDebugView;
        globalIllumination.bDirectOnly = settings.Debug.bGiDirectOnly;

        if (bRendererSettingsChanged)
        {
            // One frame-boundary wait covers the main scene and every offscreen
            // scene. SceneRenderer::SetRenderSettings coalesces AA+GI rebuilds.
            mImpl->Device.WaitForIdle();
            mImpl->SceneDesc.AntiAliasing = antiAliasing;
            mImpl->SceneDesc.GlobalIllumination = globalIllumination;
            mImpl->Scene->SetRenderSettings(
                antiAliasing, globalIllumination);
            for (Impl::OffscreenView& view : mImpl->OffscreenViews)
            {
                if (view.bLive && view.Renderer)
                {
                    view.Renderer->SetRenderSettings(
                        antiAliasing, globalIllumination);
                }
            }
            mImpl->bMainOffscreenContentValid = false;
        }

        if (bPbrDebugChanged || bShadowDebugChanged)
        {
            const auto applyDebug = [&settings, bPbrDebugChanged,
                    bShadowDebugChanged](SceneRenderer& renderer)
            {
                if (bPbrDebugChanged)
                {
                    renderer.SetPbrDebugMode(static_cast<ScenePbrDebugMode>(
                        settings.Debug.PbrDebugMode));
                }
                if (bShadowDebugChanged)
                {
                    renderer.SetDirectionalShadowCascadeDebug(
                        settings.Debug.bShadowCascadeDebug);
                }
            };
            applyDebug(*mImpl->Scene);
            for (Impl::OffscreenView& view : mImpl->OffscreenViews)
            {
                if (view.bLive && view.Renderer)
                    applyDebug(*view.Renderer);
            }
        }
    }

    AntiAliasingSettings RuntimeRenderer::GetAntiAliasingSettings() const
    {
        return GetRequestedAntiAliasingSettings();
    }

    AntiAliasingSettings
        RuntimeRenderer::GetRequestedAntiAliasingSettings() const
    {
        if (!IsInitialized())
            return {};
        return mImpl->Scene->GetRequestedAntiAliasingSettings();
    }

    AntiAliasingSettings
        RuntimeRenderer::GetEffectiveAntiAliasingSettings() const
    {
        if (!IsInitialized())
            return {};
        return mImpl->Scene->GetEffectiveAntiAliasingSettings();
    }

    AntiAliasingCapabilities
        RuntimeRenderer::GetAntiAliasingCapabilities() const
    {
        if (!IsInitialized())
            return {};
        return mImpl->Scene->GetAntiAliasingCapabilities();
    }

    AntiAliasingSelection RuntimeRenderer::GetAntiAliasingSelection() const
    {
        if (!IsInitialized())
            return {};
        return mImpl->Scene->GetAntiAliasingSelection();
    }

    void RuntimeRenderer::SetGlobalIlluminationSettings(
        const GlobalIlluminationSettings& settings)
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer GI configuration requires Initialize");
        mImpl->Device.WaitForIdle();
        mImpl->SceneDesc.GlobalIllumination = settings;
        mImpl->Scene->SetGlobalIlluminationSettings(settings);
        for (Impl::OffscreenView& view : mImpl->OffscreenViews)
        {
            if (view.bLive && view.Renderer)
                view.Renderer->SetGlobalIlluminationSettings(settings);
        }
        mImpl->bMainOffscreenContentValid = false;
    }

    void RuntimeRenderer::ResetGlobalIlluminationHistory()
    {
        if (!IsInitialized())
            return;
        mImpl->Device.WaitForIdle();
        mImpl->Scene->ResetGlobalIlluminationHistory();
        for (Impl::OffscreenView& view : mImpl->OffscreenViews)
        {
            if (view.bLive && view.Renderer)
                view.Renderer->ResetGlobalIlluminationHistory();
        }
        mImpl->bMainOffscreenContentValid = false;
    }

    GlobalIlluminationSettings RuntimeRenderer::GetGlobalIlluminationSettings() const
    {
        return GetRequestedGlobalIlluminationSettings();
    }

    GlobalIlluminationSettings
        RuntimeRenderer::GetRequestedGlobalIlluminationSettings() const
    {
        if (!IsInitialized())
            return {};
        return mImpl->Scene->GetRequestedGlobalIlluminationSettings();
    }

    GlobalIlluminationSettings
        RuntimeRenderer::GetEffectiveGlobalIlluminationSettings() const
    {
        if (!IsInitialized())
            return {};
        return mImpl->Scene->GetEffectiveGlobalIlluminationSettings();
    }

    GlobalIlluminationSelection
        RuntimeRenderer::GetGlobalIlluminationSelection() const
    {
        if (!IsInitialized())
            return {};
        return mImpl->Scene->GetGlobalIlluminationSelection();
    }

    std::vector<GlobalIlluminationTechniqueDescriptor>
        RuntimeRenderer::EnumerateGlobalIlluminationTechniques() const
    {
        if (!IsInitialized())
            return {};
        return mImpl->Scene->EnumerateGlobalIlluminationTechniques();
    }

    RenderCapabilities RuntimeRenderer::GetCapabilities() const
    {
        return IsInitialized()
            ? mImpl->Device.GetCapabilities()
            : RenderCapabilities{};
    }

    void RuntimeRenderer::ActivateOfflineViewportRenderer(
        std::string providerId)
    {
        if (!IsInitialized())
            throw std::logic_error(
                "Offline viewport renderer activation requires Initialize");
        mImpl->Device.WaitForIdle();
        mImpl->Scene->ActivateOfflineViewportRenderer(std::move(providerId));
        mImpl->bMainOffscreenContentValid = false;
    }

    void RuntimeRenderer::DeactivateOfflineViewportRenderer()
    {
        if (!IsInitialized())
            return;
        mImpl->Device.WaitForIdle();
        mImpl->Scene->DeactivateOfflineViewportRenderer();
        mImpl->bMainOffscreenContentValid = false;
    }

    std::string RuntimeRenderer::GetActiveOfflineViewportRendererId() const
    {
        return IsInitialized()
            ? mImpl->Scene->GetActiveOfflineViewportRendererId()
            : std::string{};
    }

    IOfflineViewportRendererSession*
        RuntimeRenderer::GetOfflineViewportRendererSession() const noexcept
    {
        return IsInitialized()
            ? mImpl->Scene->GetOfflineViewportRendererSession()
            : nullptr;
    }

    std::vector<OfflineViewportRendererProviderDescriptor>
        RuntimeRenderer::EnumerateOfflineViewportRenderers() const
    {
        return IsInitialized()
            ? mImpl->Scene->EnumerateOfflineViewportRenderers()
            : std::vector<OfflineViewportRendererProviderDescriptor>{};
    }

    RuntimeEnvironmentPreparation RuntimeRenderer::PrepareEnvironment(std::string_view path)
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer environment preparation requires Initialize");
        return ToRuntimeEnvironmentPreparation(mImpl->Scene->PrepareEnvironment(path));
    }

    RuntimeEnvironmentPreparation RuntimeRenderer::GetEnvironmentPreparation(
        std::string_view path) const
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer environment query requires Initialize");
        return ToRuntimeEnvironmentPreparation(mImpl->Scene->GetEnvironmentPreparation(path));
    }

    bool RuntimeRenderer::IsEnvironmentReady(std::string_view path) const
    {
        return IsInitialized() && mImpl->Scene->IsEnvironmentReady(path);
    }

	ManagedTextureHandle RuntimeRenderer::LoadTexture(const TextureLoadDesc& desc)
	{
		if (!IsInitialized())
			throw std::logic_error("RuntimeRenderer texture loading requires Initialize");
		return mImpl->Scene->LoadTexture(desc);
	}

	const ManagedTexture& RuntimeRenderer::GetTexture(ManagedTextureHandle handle) const
	{
		if (!IsInitialized())
			throw std::logic_error("RuntimeRenderer texture query requires Initialize");
		return mImpl->Scene->GetTexture(handle);
	}

	void RuntimeRenderer::ReleaseTexture(
		ManagedTextureHandle handle,
		uint64_t retireSubmission)
	{
		if (!IsInitialized())
			throw std::logic_error("RuntimeRenderer texture release requires Initialize");
		mImpl->Scene->ReleaseTexture(handle, retireSubmission);
	}

	void RuntimeRenderer::EnableMainOffscreenOutput()
	{
		if (!IsInitialized())
			throw std::logic_error("RuntimeRenderer main offscreen output requires Initialize");
		const uint32_t width = GetWidth();
		const uint32_t height = GetHeight();
		const SceneRendererOffscreenOutput current = mImpl->Scene->GetOffscreenOutput();
		if (mImpl->bMainOffscreenEnabled && current &&
			current.Width == width && current.Height == height)
		{
			return;
		}
		mImpl->Device.WaitForIdle();
		mImpl->Scene->ConfigureOffscreenOutput(width, height, "Runtime Main Scene Viewport");
		mImpl->bMainOffscreenEnabled = true;
		mImpl->bMainOffscreenContentValid = false;
	}

	RuntimeOffscreenOutput RuntimeRenderer::GetMainOffscreenOutput() const
	{
		// Editor UI must not register or sample a newly allocated Vulkan image
		// before SceneRenderer has recorded its first offscreen output.
		if (!IsInitialized() || !mImpl->bMainOffscreenEnabled ||
			!mImpl->bMainOffscreenContentValid)
			return {};
		return ToRuntimeOffscreenOutput(mImpl->Scene->GetOffscreenOutput());
	}

	RuntimeOffscreenViewHandle RuntimeRenderer::CreateOffscreenView(
		uint32_t width,
		uint32_t height,
		std::string debugName)
	{
		if (!IsInitialized() || width == 0 || height == 0)
			throw std::invalid_argument("RuntimeRenderer offscreen view requires valid dimensions");
		uint32_t index = 0;
		if (!mImpl->FreeOffscreenViews.empty())
		{
			index = mImpl->FreeOffscreenViews.back();
			mImpl->FreeOffscreenViews.pop_back();
		}
		else
		{
			if (mImpl->OffscreenViews.size() >= std::numeric_limits<uint32_t>::max())
				throw std::length_error("RuntimeRenderer offscreen view capacity exceeded");
			index = static_cast<uint32_t>(mImpl->OffscreenViews.size());
			mImpl->OffscreenViews.emplace_back();
		}

		Impl::OffscreenView& view = mImpl->OffscreenViews[index];
		try
		{
			view.DebugName = std::move(debugName);
			SceneRendererDesc sceneDesc = mImpl->SceneDesc;
			sceneDesc.bEnableGpuTiming = false;
			view.Renderer = CreateScope<SceneRenderer>(sceneDesc);
			view.Renderer->ConfigureOffscreenOutput(width, height, view.DebugName);
			view.bLive = true;
			return { index, view.Generation };
		}
		catch (...)
		{
			view.Renderer.reset();
			view.World.reset();
			view.DebugName.clear();
			mImpl->FreeOffscreenViews.push_back(index);
			throw;
		}
	}

	void RuntimeRenderer::ResizeOffscreenView(
		RuntimeOffscreenViewHandle handle,
		uint32_t width,
		uint32_t height)
	{
		if (!IsInitialized() || !handle || handle.Index >= mImpl->OffscreenViews.size())
			throw std::out_of_range("Invalid runtime offscreen view handle");
		Impl::OffscreenView& view = mImpl->OffscreenViews[handle.Index];
		if (!view.bLive || view.Generation != handle.Generation || !view.Renderer)
			throw std::out_of_range("Stale runtime offscreen view handle");
		if (width == 0 || height == 0)
			throw std::invalid_argument("Runtime offscreen view dimensions must be non-zero");
		if (view.PendingCapture)
			throw std::logic_error("Cannot resize an offscreen view with a queued capture");
		const SceneRendererOffscreenOutput current = view.Renderer->GetOffscreenOutput();
		if (current.Width == width && current.Height == height)
			return;
		mImpl->Device.WaitForIdle();
		view.Renderer->ConfigureOffscreenOutput(width, height, view.DebugName);
	}

	void RuntimeRenderer::SubmitOffscreenView(
		RuntimeOffscreenViewHandle handle,
		Ref<const RenderWorld> world)
	{
		if (!IsInitialized() || !handle || handle.Index >= mImpl->OffscreenViews.size())
			throw std::out_of_range("Invalid runtime offscreen view handle");
		Impl::OffscreenView& view = mImpl->OffscreenViews[handle.Index];
		if (!view.bLive || view.Generation != handle.Generation || !view.Renderer)
			throw std::out_of_range("Stale runtime offscreen view handle");
		if (world)
		{
			const SceneEnvironmentPreparation environment =
				view.Renderer->GetEnvironmentPreparation(world->Environment.HdrTexturePath);
			if (environment.State == SceneEnvironmentState::Unrequested)
				view.Renderer->PrepareEnvironment(world->Environment.HdrTexturePath);
		}
		view.World = std::move(world);
		view.bSubmitted = static_cast<bool>(view.World);
	}

	RuntimeOffscreenOutput RuntimeRenderer::GetOffscreenOutput(
		RuntimeOffscreenViewHandle handle) const
	{
		if (!IsInitialized() || !handle || handle.Index >= mImpl->OffscreenViews.size())
			return {};
		const Impl::OffscreenView& view = mImpl->OffscreenViews[handle.Index];
		if (!view.bLive || view.Generation != handle.Generation || !view.Renderer)
			return {};
		return ToRuntimeOffscreenOutput(view.Renderer->GetOffscreenOutput());
	}

	TextureReadbackHandle RuntimeRenderer::RequestOffscreenCapture(
		RuntimeOffscreenViewHandle handle,
		std::filesystem::path outputPath)
	{
		if (!IsInitialized() || !handle || handle.Index >= mImpl->OffscreenViews.size())
			throw std::out_of_range("Invalid runtime offscreen view handle");
		if (outputPath.empty())
			throw std::invalid_argument("Runtime offscreen capture output path is empty");
		Impl::OffscreenView& view = mImpl->OffscreenViews[handle.Index];
		if (!view.bLive || view.Generation != handle.Generation || !view.Renderer)
			throw std::out_of_range("Stale runtime offscreen view handle");
		if (view.PendingCapture && mImpl->Readbacks->IsValid(view.PendingCapture))
			throw std::logic_error("Runtime offscreen view already has a queued capture");

		view.PendingCapture = mImpl->Readbacks->Reserve(TextureReadbackDesc{
			.OutputPath = std::move(outputPath),
		});
		return view.PendingCapture;
	}

	void RuntimeRenderer::DestroyOffscreenView(RuntimeOffscreenViewHandle handle)
	{
		if (!IsInitialized() || !handle || handle.Index >= mImpl->OffscreenViews.size())
			return;
		Impl::OffscreenView& view = mImpl->OffscreenViews[handle.Index];
		if (!view.bLive || view.Generation != handle.Generation)
			return;
		if (view.PendingCapture && mImpl->Readbacks->IsValid(view.PendingCapture))
			mImpl->Readbacks->CancelPending(view.PendingCapture);
		view.PendingCapture = {};
		mImpl->Device.WaitForIdle();
		view.Renderer.reset();
		view.World.reset();
		view.DebugName.clear();
		view.bLive = false;
		view.bSubmitted = false;
		++view.Generation;
		if (view.Generation == 0)
			view.Generation = 1;
		mImpl->FreeOffscreenViews.push_back(handle.Index);
	}

    void RuntimeRenderer::CollectCaptures(uint64_t completedSubmission)
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer capture collection requires Initialize");
        mImpl->Readbacks->Collect(completedSubmission);
    }

    bool RuntimeRenderer::IsCaptureValid(TextureReadbackHandle handle) const noexcept
    {
        return IsInitialized() && mImpl->Readbacks->IsValid(handle);
    }

    TextureReadbackResult RuntimeRenderer::GetCaptureResult(
        TextureReadbackHandle handle) const
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer capture query requires Initialize");
        return mImpl->Readbacks->GetResult(handle);
    }

    void RuntimeRenderer::ReleaseCapture(TextureReadbackHandle handle)
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer capture release requires Initialize");
        // An offscreen capture may still be reserved for the next frame. Clear
        // the owning view before invalidating that reservation so RecordFrame
        // can never observe and reuse a stale handle.
        for (Impl::OffscreenView& view : mImpl->OffscreenViews)
        {
            if (view.bLive && view.PendingCapture == handle)
                view.PendingCapture = {};
        }
        mImpl->Readbacks->Release(handle);
    }

    bool RuntimeRenderer::IsInitialized() const noexcept
    {
        return mImpl && mImpl->Device.IsInitialized() && mImpl->Scene && mImpl->Readbacks;
    }

    nvrhi::IDevice* RuntimeRenderer::GetDevice() const
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer device query requires Initialize");
        return mImpl->Device.GetDevice();
    }

    uint32_t RuntimeRenderer::GetFramesInFlight() const
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer capabilities query requires Initialize");
        return mImpl->Device.GetCapabilities().FramesInFlight;
    }

    uint32_t RuntimeRenderer::GetWidth() const
    {
        if (!IsInitialized())
            return 0;
        return mImpl->Device.GetSwapChain().GetWidth();
    }

    uint32_t RuntimeRenderer::GetHeight() const
    {
        if (!IsInitialized())
            return 0;
        return mImpl->Device.GetSwapChain().GetHeight();
    }

    RenderBackend RuntimeRenderer::GetBackend() const
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer backend query requires Initialize");
        return mImpl->Device.GetBackend();
    }

	std::string RuntimeRenderer::BuildSceneStatsSummary() const
	{
		if (!IsInitialized())
			return "renderer=uninitialized";
		const SceneRendererStats& stats = mImpl->Scene->GetStats();
		return fmt::format(
			"gpuTiming={} gpuMs={:.3f} gpuLastMs={:.3f} gpuSamples={} objects={} visible={} "
			"culled={} batches={} indirect={} instanced={} computeDispatch={} "
			"cpuFallback={} transparent={} skinned={} shadowDraws={} "
			"cascades={} materials={} "
			"bindlessTextures={} materialPages={} materialPagePeakTextures={} "
			"materialBindingSets={} environmentReady={} passes={} "
            "giTechnique={} giSamples={} rtInstances={} blasBuild={} blasReuse={} "
            "tlasBuild={} tlasUpdate={} rtExcluded={} rtGeometryBytes={} "
            "rtTableBytes={} rgPersistentBytes={} rgTransientReservedBytes={} "
            "rgPeakFrameTransientBytes={} rtEmissiveTriangles={}",
			stats.bGpuTimingEnabled ? 1 : 0,
			stats.GpuRenderGraphAverageMilliseconds,
			stats.GpuRenderGraphLastMilliseconds,
			stats.GpuRenderGraphSampleCount,
			stats.GpuDrivenObjectCount,
			stats.GpuVisibleObjectCount,
			stats.GpuCulledObjectCount,
			stats.GpuDrivenBatchCount,
			stats.GpuIndirectDrawCount,
			stats.GpuInstancedBatchCount,
			stats.GpuComputeDispatchCount,
			stats.CpuFallbackDrawCount,
			stats.CpuTransparentDrawCount,
			stats.SkinnedDrawCount,
			stats.DirectionalShadowDrawCount,
			stats.DirectionalShadowCascadeCount,
			stats.GpuMaterialCount,
			stats.BindlessTextureCount,
			stats.MaterialDescriptorPageCount,
			stats.MaterialDescriptorPeakPageTextures,
			stats.MaterialBindingSetCreationCount,
			stats.bEnvironmentReady ? 1 : 0,
			stats.CompiledPasses.size(),
            static_cast<uint32_t>(stats.GiStats.Technique),
            stats.GiStats.AccumulatedSamples,
            stats.RayTracingInstanceCount,
            stats.RayTracingBlasBuildCount,
            stats.RayTracingBlasReuseCount,
            stats.RayTracingTlasBuildCount,
            stats.RayTracingTlasUpdateCount,
            stats.RayTracingExcludedInstanceCount,
            stats.RayTracingGeometryBytes,
            stats.RayTracingSceneTableBytes,
            stats.RenderGraphPersistentBytes,
            stats.RenderGraphTransientReservedBytes,
            stats.RenderGraphPeakFrameTransientBytes,
            stats.RayTracingEmissiveTriangleCount);
	}

	RenderGraphDiagnosticSnapshot
		RuntimeRenderer::GetRenderGraphDiagnosticSnapshot() const
	{
		return IsInitialized()
			? mImpl->Scene->GetRenderGraphDiagnosticSnapshot()
			: RenderGraphDiagnosticSnapshot{};
	}

	std::string RuntimeRenderer::BuildRenderGraphDebugDump() const
	{
		if (!IsInitialized())
			return "renderer=uninitialized";
		return mImpl->Scene->BuildRenderGraphDebugDump();
	}
}
