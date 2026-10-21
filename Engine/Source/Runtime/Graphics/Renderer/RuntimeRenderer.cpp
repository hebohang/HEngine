#include "Hepch.h"

#include "Runtime/Graphics/Renderer/RuntimeRenderer.h"
#include "Runtime/Graphics/Renderer/RejectedViewConfigurationInputs.h"

#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/Graphics/RHI/RenderSurface.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/Renderer/RenderPipelineRegistry.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"
#include "Runtime/Graphics/Settings/RenderSettings.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    namespace
    {
        // Configuration preparation reports bad parameters with invalid_argument.
        // Only the automatic frame path recovers; explicit settings transactions
        // still propagate this rejection to their caller.
        class ViewConfigurationRejected final : public std::runtime_error
        {
        public:
            using std::runtime_error::runtime_error;
        };

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
        struct ViewConfiguration
        {
            std::string BaseHash;
            std::string Kind;
            Json World = Json::object();
            Json Camera = Json::object();
            std::optional<RenderSettings> Effective;
            RenderDebugSettings Debug;
            RenderCaptureSettings Capture;
        };
		struct OffscreenView
		{
			Scope<RenderPipeline> Renderer;
            ViewConfiguration Configuration;
			Ref<const RenderWorld> World;
			std::string DebugName;
			TextureReadbackHandle PendingCapture;
			uint32_t Generation = 1;
			uint32_t PendingWidth = 0;
			uint32_t PendingHeight = 0;
			bool bLive = false;
			bool bSubmitted = false;
            bool bReplacementOutputPending = false;
            SceneRendererOffscreenOutput PreviousOutput;
            uint64_t OutputGenerationOffset = 0;
            uint64_t PendingOutputGenerationOffset = 0;
		};

        RenderDevice Device;
        FrameContext* ActiveSimulationFrame = nullptr;
        uint32_t ViewportWidth = 0;
        uint32_t ViewportHeight = 0;
        uint32_t PendingWidth = 0;
        uint32_t PendingHeight = 0;
        Ref<const RenderWorld> PreparationWorld;
        std::vector<std::pair<std::string, std::function<void()>>> BoundaryRequests;
        void RequireFrameBoundary(const char* operation) const
        {
            if (ActiveSimulationFrame)
                throw std::logic_error(std::string(operation) + " requires the boundary before BeginSimulationFrame");
        }
        Scope<ShaderManager> MaterialShaderHandles;
        Scope<MaterialShaderManager> MaterialShaders;
        Scope<RenderPipeline> Scene;
        Scope<TextureReadbackQueue> Readbacks;
		SceneRendererDesc SceneDesc;
		std::vector<OffscreenView> OffscreenViews;
		std::vector<uint32_t> FreeOffscreenViews;
        RenderWorld EmptyWorld;
        TextureReadbackHandle RecordingCapture;
        uint64_t LastSubmission = 0;
		Ref<const RenderWorld> LastMainWorld;
        RuntimeConfigurationPreparationStats ConfigurationPreparationStats;
        ViewConfiguration MainConfiguration;
        std::optional<ResolvedRenderSettings> BaseSettings;
        std::optional<RejectedViewConfigurationInputs> RejectedAutomaticInputs;

        bool MatchesRejectedAutomaticInputs(const RenderWorld* mainWorld) const
        {
            if (!RejectedAutomaticInputs || !BaseSettings ||
                !RejectedAutomaticInputs->MatchesBase(BaseSettings->Hash)) return false;
            const auto& rejected = *RejectedAutomaticInputs;
            const auto& main = mainWorld ? *mainWorld : EmptyWorld;
            if (!rejected.MatchesView(0, std::numeric_limits<uint32_t>::max(), 0,
                main.SceneGeneration, mainWorld != nullptr, main.bEditorView ? "editor" : "main",
                main.RenderOverrides, main.Camera.RenderOverrides)) return false;
            size_t offset = 1;
            for (size_t slot = 0; slot < OffscreenViews.size(); ++slot)
            {
                const auto& view = OffscreenViews[slot];
                if (!view.bLive || !view.Renderer) continue;
                const auto& world = view.World ? *view.World : EmptyWorld;
                if (!rejected.MatchesView(offset++, static_cast<uint32_t>(slot), view.Generation,
                    world.SceneGeneration, static_cast<bool>(view.World), "preview",
                    world.RenderOverrides, world.Camera.RenderOverrides)) return false;
            }
            return rejected.MatchesViewCount(offset);
        }

        void RememberRejectedAutomaticInputs(const RenderWorld* mainWorld)
        {
            RejectedViewConfigurationInputs inputs;
            inputs.BaseHash = BaseSettings->Hash;
            const auto append = [&](uint32_t slot, uint32_t generation, const RenderWorld* world, const char* kind)
            {
                const auto& data = world ? *world : EmptyWorld;
                inputs.Views.push_back({slot, generation, data.SceneGeneration, world != nullptr,
                    kind, data.RenderOverrides, data.Camera.RenderOverrides});
            };
            append(std::numeric_limits<uint32_t>::max(), 0, mainWorld,
                mainWorld && mainWorld->bEditorView ? "editor" : "main");
            for (size_t slot = 0; slot < OffscreenViews.size(); ++slot)
            {
                const auto& view = OffscreenViews[slot];
                if (view.bLive && view.Renderer)
                    append(static_cast<uint32_t>(slot), view.Generation, view.World.get(), "preview");
            }
            RejectedAutomaticInputs = std::move(inputs);
        }

        struct RetiredPipeline
        {
            uint64_t Submission;
            Scope<RenderPipeline> Pipeline;
        };
        std::vector<RetiredPipeline> RetiredPipelines;

        void ApplyViewConfigurations(const RenderWorld& mainWorld,
            const ResolvedRenderSettings& settings, const SceneRendererDesc& defaults)
        {
            struct Candidate
            {
                RenderPipeline* Pipeline;
                ViewConfiguration* State;
                ViewConfiguration Next;
                bool bPostConstantsOnly = false;
                bool bUnchanged = false;
                Scope<RenderPipeline>* Owner = nullptr;
                Scope<RenderPipeline> Replacement;
                OffscreenView* View = nullptr;
                SceneRendererOffscreenOutput PriorOutput;
                uint64_t NextOutputGenerationOffset = 0;
            };
            std::vector<Candidate> candidates;
            auto& stats = ConfigurationPreparationStats;
            const auto start = std::chrono::steady_clock::now();
            const auto prepareUnchecked = [&](Scope<RenderPipeline>& owner, ViewConfiguration& state,
                const RenderWorld& world, const char* kind, OffscreenView* view = nullptr)
            {
                auto& pipeline = *owner;
                if (state.BaseHash == settings.Hash && state.Kind == kind &&
                    state.World == world.RenderOverrides && state.Camera == world.Camera.RenderOverrides) return;
                Json camera = Json::object();
                if (settings.Effective.ViewOverrides.contains(kind))
                    camera = settings.Effective.ViewOverrides.at(kind);
                camera.update(world.Camera.RenderOverrides);
                const auto resolved = ResolveRenderViewSettings(settings, world.RenderOverrides, camera);
                if (HasDiagnosticErrors(resolved.Diagnostics))
                {
                    std::string message = "View rendering override rejected";
                    for (const auto& diagnostic : resolved.Diagnostics)
                        if (diagnostic.Severity == RenderSettingDiagnosticSeverity::Error)
                            message += "; " + diagnostic.PropertyId + " [" + diagnostic.Code + "]: " + diagnostic.Message;
                    throw std::invalid_argument(message);
                }
                auto target = defaults;
                target.bEnableGpuTiming = defaults.bEnableGpuTiming && std::string_view(kind) != "preview";
                target.bEnableCpuGraphTiming = defaults.bEnableCpuGraphTiming && std::string_view(kind) != "preview";
                const auto& effective = resolved.Effective;
                target.PipelineType = effective.PipelineType;
                target.PipelineFeatures = effective.PipelineFeatures;
                target.Path = effective.Path;
                target.AntiAliasing = effective.AntiAliasing;
                target.Upscaling = effective.Upscaling;
                target.TextureStreamingBudgetBytes = uint64_t(effective.TextureStreamingBudgetMiB) * 1024 * 1024;
                target.AntiAliasing.CaptureSupersampleScale = resolved.Capture.SupersampleScale;
                target.GlobalIllumination = effective.GlobalIllumination;
                target.GlobalIllumination.DebugView = resolved.Debug.GiDebugView;
                target.GlobalIllumination.bDirectOnly = resolved.Debug.bGiDirectOnly;
                target.PostProcess = effective.PostProcess;
                target.Clouds = effective.Clouds;
                target.AmbientOcclusion = effective.AmbientOcclusion;
                target.Reflections = effective.Reflections;
                target.bGpuOcclusionEnabled = effective.bGpuOcclusionEnabled;
                target.bGpuBatchingEnabled = effective.bGpuBatchingEnabled;
                target.bGpuTightFrustumEnabled = effective.bGpuTightFrustumEnabled;
                target.bGpuShadowBatchingEnabled = effective.bGpuShadowBatchingEnabled;
                target.bTransientTextureReuse = effective.bTransientTextureReuse;
                target.bLodEnabled = effective.bLodEnabled;
                target.bShadowsEnabled = effective.Shadows.bEnabled;
                target.bContinuousPcfEnabled = effective.Shadows.bContinuousPcfEnabled;
                target.LodMaxPixelError = effective.LodMaxPixelError;
                // Register ownership before preparation so every failure path aborts it.
                bool bUnchanged = false;
                bool bPostOnly = false;
                if (state.Effective || BaseSettings)
                {
                    auto before = state.Effective ? *state.Effective : BaseSettings->Effective;
                    auto after = effective;
                    before.PipelineProfile.clear(); after.PipelineProfile.clear();
                    before.TransientCacheBudgetMiB = after.TransientCacheBudgetMiB;
                    before.TextureStreamingBudgetMiB = after.TextureStreamingBudgetMiB;
                    before.ViewOverrides = {}; after.ViewOverrides = {};
                    bUnchanged = before == after;
                    const auto needsPost = [](const ScenePostProcessSettings& post) {
                        return post.BloomIntensity > 0.0f || post.bAutoExposure ||
                            post.Contrast != 1.0f || post.Saturation != 1.0f || post.Temperature != 0.0f;
                    };
                    const bool bSamePostStructure = needsPost(before.PostProcess) == needsPost(after.PostProcess) &&
                        (before.PostProcess.BloomIntensity > 0.0f) == (after.PostProcess.BloomIntensity > 0.0f) &&
                        before.PostProcess.bAutoExposure == after.PostProcess.bAutoExposure;
                    before.PostProcess = after.PostProcess;
                    before.Upscaling.Sharpness = after.Upscaling.Sharpness;
                    bPostOnly = effective.PipelineType == "realtime" && bSamePostStructure && before == after;
                    const auto& previousDebug = state.Effective ? state.Debug : BaseSettings->Debug;
                    const auto& previousCapture = state.Effective ? state.Capture : BaseSettings->Capture;
                    if (previousDebug.GiDebugView != resolved.Debug.GiDebugView ||
                        previousDebug.bGiDirectOnly != resolved.Debug.bGiDirectOnly ||
                        previousCapture.SupersampleScale != resolved.Capture.SupersampleScale)
                    {
                        bUnchanged = false;
                        bPostOnly = false;
                    }
                }
                candidates.push_back({&pipeline, &state, {settings.Hash, kind,
                    world.RenderOverrides, world.Camera.RenderOverrides, effective, resolved.Debug, resolved.Capture}, bPostOnly, bUnchanged});
                auto& candidate = candidates.back();
                candidate.View = view;
                const auto& previousType = state.Effective ? state.Effective->PipelineType : SceneDesc.PipelineType;
                if (previousType != effective.PipelineType)
                {
                    candidate.Owner = &owner;
                    candidate.Replacement = CreateRenderPipeline(target);
                    const auto output = pipeline.GetOffscreenOutput();
                    candidate.PriorOutput = output;
                    if (view)
                        candidate.NextOutputGenerationOffset = view->OutputGenerationOffset +
                            std::max(output.Generation, view->PreviousOutput.Generation) + 1;
                    if (output.Width && output.Height)
                        candidate.Replacement->ConfigureOffscreenOutput(output.Width, output.Height, kind, output.ColorFormat);
                    candidate.Replacement->SetPbrDebugMode(static_cast<ScenePbrDebugMode>(resolved.Debug.PbrDebugMode));
                    candidate.Replacement->SetDirectionalShadowCascadeDebug(resolved.Debug.bShadowCascadeDebug);
                    candidate.Pipeline = candidate.Replacement.get();
                }
                if (bPostOnly || bUnchanged) return;
                SceneRendererFrameDesc preparationFrame;
                if (candidate.Replacement)
                {
                    auto& swapChain = Device.GetSwapChain();
                    preparationFrame.BackBuffer = swapChain.GetCurrentBackBuffer();
                    preparationFrame.BackBufferFormat = swapChain.GetFormat();
                    preparationFrame.BackBufferInitialState = swapChain.GetCurrentBackBufferInitialState();
                    preparationFrame.Width = view ? candidate.PriorOutput.Width : swapChain.GetWidth();
                    preparationFrame.Height = view ? candidate.PriorOutput.Height : swapChain.GetHeight();
                    preparationFrame.OutputMode = view ? SceneRendererOutputMode::OffscreenOnly :
                        (bMainOffscreenEnabled ? SceneRendererOutputMode::SwapChainAndOffscreen : SceneRendererOutputMode::SwapChain);
                }
                const auto result = candidate.Pipeline->PrepareRenderingConfiguration(target, world,
                    candidate.Replacement ? &preparationFrame : nullptr);
                if (candidate.Replacement && !result.bGraphPrepared)
                    throw std::runtime_error("Replacement pipeline did not prepare its output graph before publication");
                stats.PreparedViews += result.bGraphPrepared ? 1u : 0u;
                stats.PreparedCandidatePoolNominalBytes += result.CandidatePoolNominalBytes;
                stats.PreparationMilliseconds += result.PreparationMilliseconds;
            };
            const auto prepare = [&](Scope<RenderPipeline>& owner, ViewConfiguration& state,
                const RenderWorld& world, const char* kind, OffscreenView* view = nullptr)
            {
                try { prepareUnchecked(owner, state, world, kind, view); }
                catch (const std::invalid_argument& error)
                {
                    const std::string identity = view
                        ? std::string(kind) + "[" + std::to_string(view - OffscreenViews.data()) + "]"
                        : kind;
                    throw ViewConfigurationRejected(identity + ": " + error.what());
                }
            };
            try
            {
                prepare(Scene, MainConfiguration, mainWorld, mainWorld.bEditorView ? "editor" : "main");
                for (auto& view : OffscreenViews)
                    if (view.bLive && view.Renderer)
                        prepare(view.Renderer, view.Configuration, view.World ? *view.World : EmptyWorld, "preview", &view);
                RetiredPipelines.reserve(RetiredPipelines.size() + candidates.size());
            }
            catch (...)
            {
                for (auto& candidate : candidates)
                    if (!candidate.bPostConstantsOnly && !candidate.bUnchanged)
                        candidate.Pipeline->AbortPreparedRenderingConfiguration();
                stats.bCommitted = false;
                throw;
            }
            for (auto& candidate : candidates)
            {
                if (candidate.bPostConstantsOnly && !candidate.bUnchanged)
                {
                    candidate.Pipeline->SetPostProcessSettings(candidate.Next.Effective->PostProcess);
                    candidate.Pipeline->SetUpscalingSharpness(candidate.Next.Effective->Upscaling.Sharpness);
                }
                else if (!candidate.bUnchanged)
                    candidate.Pipeline->CommitPreparedRenderingConfiguration();
                candidate.Pipeline->SetTextureStreamingBudget(uint64_t(candidate.Next.Effective->TextureStreamingBudgetMiB) * 1024 * 1024);
                candidate.Pipeline->SetTransientCacheBudget(candidate.Next.Effective->TransientCacheBudgetMiB == 0
                    ? std::numeric_limits<uint64_t>::max()
                    : uint64_t(candidate.Next.Effective->TransientCacheBudgetMiB) * 1024u * 1024u);
                if (candidate.Replacement)
                {
                    if (candidate.View)
                    {
                        if (!candidate.View->bReplacementOutputPending)
                            candidate.View->PreviousOutput = candidate.PriorOutput;
                        candidate.View->bReplacementOutputPending = true;
                        candidate.View->PendingOutputGenerationOffset = candidate.NextOutputGenerationOffset;
                    }
                    RetiredPipelines.push_back({LastSubmission, std::move(*candidate.Owner)});
                    *candidate.Owner = std::move(candidate.Replacement);
                }
                if (candidate.View && !candidate.bUnchanged)
                    candidate.View->bSubmitted = static_cast<bool>(candidate.View->World);
                *candidate.State = std::move(candidate.Next);
            }
            if (!candidates.empty())
            {
                ++stats.AttemptId;
                stats.bCommitted = true;
                stats.TransactionMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                bMainOffscreenContentValid = false;
            }
        }
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

    void RuntimeRenderer::Initialize(const RuntimeRendererDesc& requestedDesc)
    {
        RuntimeRendererDesc desc = requestedDesc;
        if (mImpl)
            throw std::logic_error("RuntimeRenderer is already initialized");
        if (!desc.Surface)
            throw std::invalid_argument("RuntimeRenderer requires a RenderSurface");
        if (desc.ShaderCacheDirectory.empty())
            throw std::invalid_argument("RuntimeRenderer requires a shader cache directory");
        desc.ShaderCacheDirectory = ShaderArtifactResolver(
            desc.ShaderCacheDirectory, desc.Backend).GetArtifactDirectory();

        Scope<Impl> impl = CreateScope<Impl>();
        try
        {
            impl->Device.Initialize(
                desc.Backend,
                *desc.Surface,
                desc.bEnableValidation,
                desc.AdapterSelection,
                desc.FramesInFlight);
            impl->ViewportWidth = impl->Device.GetSwapChain().GetWidth();
            impl->ViewportHeight = impl->Device.GetSwapChain().GetHeight();
            impl->Device.SetSubmissionTimingEnabled(desc.bEnableGpuTiming && desc.bEnableGpuFrameMarkers);
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
                .PipelineType = desc.PipelineType,
                .PipelineFeatures = desc.PipelineFeatures,
                .TransientCacheBudgetBytes = desc.TransientCacheBudgetBytes,
                .TextureStreamingBudgetBytes = desc.TextureStreamingBudgetBytes,
                .Device = impl->Device.GetDevice(),
                .FramesInFlight = framesInFlight,
                .ShaderCacheDirectory = desc.ShaderCacheDirectory,
                .AssetRootDirectory = desc.AssetRootDirectory,
                .EngineAssetRootDirectory = desc.EngineAssetRootDirectory,
                .SharedMaterialShaderManager = impl->MaterialShaders.get(),
                .SharedMaterialShaderHandleManager = impl->MaterialShaderHandles.get(),
                .Backend = desc.Backend,
                .Path = desc.Path,
                .Capabilities = impl->Device.GetCapabilities(),
                .GlobalIllumination = desc.GlobalIllumination,
                .AntiAliasing = desc.AntiAliasing,
                .Upscaling = desc.Upscaling,
                .PostProcess = desc.PostProcess,
                .AmbientOcclusion = desc.AmbientOcclusion,
                .Clouds = desc.Clouds,
                .Reflections = desc.Reflections,
                .bGpuOcclusionEnabled = desc.bGpuOcclusionEnabled,
                .bGpuBatchingEnabled = desc.bGpuBatchingEnabled,
                .bGpuTightFrustumEnabled = desc.bGpuTightFrustumEnabled,
                .bGpuShadowBatchingEnabled = desc.bGpuShadowBatchingEnabled,
                .bTransientTextureReuse = desc.bTransientTextureReuse,
                .bLodEnabled = desc.bLodEnabled,
                .bShadowsEnabled = desc.bShadowsEnabled,
                .bContinuousPcfEnabled = desc.bContinuousPcfEnabled,
                .LodMaxPixelError = desc.LodMaxPixelError,
                .bHeapDirectlyIndexed =
                    impl->Device.GetCapabilities().HeapDirectlyIndexed.bEnabled,
                .bEnableGpuTiming = desc.bEnableGpuTiming,
                .bEnableCpuGraphTiming = desc.bEnableCpuGraphTiming,
            };
            impl->Scene = CreateRenderPipeline(impl->SceneDesc);
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
        if (RenderSettingsService::GetInstance().IsInitialized())
            mImpl->BaseSettings = RenderSettingsService::GetInstance().GetSnapshot();
    }

    void RuntimeRenderer::Shutdown()
    {
        if (!mImpl)
            return;

        AbortSimulationFrame();

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
        mImpl->RetiredPipelines.clear();
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
        mImpl->RequireFrameBoundary("Material shader refresh");
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

    void RuntimeRenderer::BeginSimulationFrame()
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer simulation frame requires Initialize");
        if (mImpl->ActiveSimulationFrame)
            throw std::logic_error("RuntimeRenderer simulation frame is already active");
        auto requests = std::move(mImpl->BoundaryRequests);
        mImpl->BoundaryRequests.clear();
        for (auto& [name, execute] : requests)
        {
            try { execute(); }
            catch (const ViewConfigurationRejected& error)
            {
                HE_CORE_ERROR("Deferred renderer operation '{}' was rejected: {}", name, error.what());
            }
            catch (const std::logic_error& error)
            {
                HE_CORE_ERROR("Deferred renderer operation '{}' was rejected: {}", name, error.what());
            }
        }
        if (mImpl->PendingWidth && mImpl->PendingHeight)
        {
            const auto width = mImpl->PendingWidth;
            const auto height = mImpl->PendingHeight;
            Resize(width, height);
            mImpl->PendingWidth = mImpl->PendingHeight = 0;
        }
        for (uint32_t index = 0; index < mImpl->OffscreenViews.size(); ++index)
        {
            auto& view = mImpl->OffscreenViews[index];
            if (view.bLive && view.PendingWidth && view.PendingHeight)
            {
                ResizeOffscreenView({index, view.Generation}, view.PendingWidth, view.PendingHeight);
                view.PendingWidth = view.PendingHeight = 0;
            }
        }
        // World overrides and resource preparation use the preceding submitted snapshot.
        if (mImpl->BaseSettings && !mImpl->MatchesRejectedAutomaticInputs(mImpl->PreparationWorld.get()))
        {
            try
            {
                mImpl->ApplyViewConfigurations(mImpl->PreparationWorld ? *mImpl->PreparationWorld : mImpl->EmptyWorld,
                    *mImpl->BaseSettings, mImpl->SceneDesc);
                mImpl->RejectedAutomaticInputs.reset();
            }
            catch (const ViewConfigurationRejected& error)
            {
                // ApplyViewConfigurations has already aborted every candidate.
                // Identical input bypasses all preparation on subsequent frames.
                mImpl->RememberRejectedAutomaticInputs(mImpl->PreparationWorld.get());
                HE_CORE_ERROR("RuntimeRenderer: retaining all previous view configurations: {0}", error.what());
            }
        }
		if (mImpl->PreparationWorld && !mImpl->PreparationWorld->Environment.HdrTexturePath.empty())
		{
			const SceneEnvironmentPreparation preparation =
				mImpl->Scene->GetEnvironmentPreparation(
					mImpl->PreparationWorld->Environment.HdrTexturePath);
			if (preparation.State == SceneEnvironmentState::Unrequested)
			{
				const SceneEnvironmentPreparation result =
					mImpl->Scene->PrepareEnvironment(
						mImpl->PreparationWorld->Environment.HdrTexturePath);
				if (result.State == SceneEnvironmentState::Failed)
				{
					HE_CORE_ERROR(
						"Failed to prepare scene environment '{}': {}",
						result.Path,
						result.Error);
				}
			}
		}

        if (mImpl->PreparationWorld) mImpl->Scene->PrepareReflectionProbeResources(*mImpl->PreparationWorld);
        FrameContext& frame = mImpl->Device.BeginFrame();
        mImpl->ActiveSimulationFrame = &frame;
        try
        {
            EffekseerRuntime::BeginHostFrame(frame.FrameIndex,
                frame.CompletedSubmissionInstance, frame.GraphicsCommandList);
        }
        catch (...)
        {
            AbortSimulationFrame();
            throw;
        }
    }

    void RuntimeRenderer::AbortSimulationFrame() noexcept
    {
        if (!mImpl || !mImpl->ActiveSimulationFrame) return;
        FrameContext& frame = *mImpl->ActiveSimulationFrame;
        mImpl->ActiveSimulationFrame = nullptr;
        if (frame.bSubmissionIssued)
        {
            EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
            mImpl->LastSubmission = frame.SubmissionInstance;
        }
        else
        {
            EffekseerRuntime::AbortHostFrame(frame.FrameIndex);
            mImpl->Device.AbortFrame(frame);
        }
    }

    RuntimeRendererFrameResult RuntimeRenderer::RenderFrame(
        const RuntimeRendererFrameDesc& desc,
        bool bVSync)
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer RenderFrame called before Initialize");
        if (mImpl->RecordingCapture)
            throw std::logic_error("RuntimeRenderer retained an unsubmitted capture from a prior frame");
        mImpl->PreparationWorld = desc.World;
        if (desc.CaptureSource != FrameCaptureSource::Automatic &&
            (desc.CapturePath.empty() || desc.CapturePath.extension() != ".raw"))
            throw std::invalid_argument("Explicit native capture sources require a .raw output path");
        if (!mImpl->ActiveSimulationFrame) BeginSimulationFrame();
        FrameContext& frame = *mImpl->ActiveSimulationFrame;
        SwapChain& swapChain = mImpl->Device.GetSwapChain();
        IRuntimeOverlayRecorder* const overlayRecorder = desc.OverlayRecorder;
        const uint64_t completedSubmission = frame.CompletedSubmissionInstance;
        std::erase_if(mImpl->RetiredPipelines, [completedSubmission](const auto& retired) {
            return retired.Submission <= completedSubmission;
        });
        const std::filesystem::path capturePath = desc.CapturePath;
        std::string captureExtension = capturePath.extension().string();
        std::transform(captureExtension.begin(), captureExtension.end(), captureExtension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        const bool bRawCapture = captureExtension == ".raw";
        const bool bLinearCapture = captureExtension == ".pfm" || bRawCapture;
        const FrameCaptureSource captureSource = desc.CaptureSource;
        Json capturedTextureMetadata;
        std::string captureError;
        const auto recordTextureMetadata = [&](nvrhi::ITexture* texture) {
            if (!texture) throw std::runtime_error("Requested native capture source is unavailable");
            const auto& textureDesc = texture->getDesc();
            capturedTextureMetadata = {
                { "source", FrameCaptureSourceName(captureSource) },
                { "format", nvrhi::getFormatInfo(textureDesc.format).name },
                { "dimensions", { textureDesc.width, textureDesc.height } },
                { "resourceName", textureDesc.debugName },
            };
        };
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
			mImpl->LastMainWorld.get() == desc.World.get() && !bLinearCapture &&
            !mImpl->Scene->RequiresContinuousUpdate() &&
            (!overlayRecorder || !overlayRecorder->HasDynamicSceneOverlay());
		auto publishSubmittedFrame = [&]()
		{
			if (!frame.bSubmissionIssued)
				return;

            // Publish retirement before any callback that can throw.
            EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
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
                if (mImpl->OffscreenViews[index].bReplacementOutputPending)
                    mImpl->OffscreenViews[index].OutputGenerationOffset = mImpl->OffscreenViews[index].PendingOutputGenerationOffset;
                mImpl->OffscreenViews[index].bReplacementOutputPending = false;
                mImpl->OffscreenViews[index].PreviousOutput = {};
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
							.WorldSnapshot = view.World,
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
                    .CaptureRecorder = !capturePath.empty() && !bLinearCapture
                        ? SceneRendererCaptureRecorder(
                            [impl = mImpl.get(), capturePath, &recordTextureMetadata](
                                const SceneRendererCaptureContext& context)
                            {
                                recordTextureMetadata(context.BackBuffer);
                                impl->RecordingCapture = impl->Readbacks->Record(
                                    context.CommandList,
                                    context.BackBuffer,
                                    TextureReadbackDesc{ .OutputPath = capturePath });
                            })
                        : SceneRendererCaptureRecorder{},
                    .LinearHdrCaptureRecorder = bLinearCapture
                        ? SceneRendererLinearHdrCaptureRecorder(
                            [impl = mImpl.get(), capturePath, bRawCapture, captureSource, &recordTextureMetadata, &captureError](
                                const SceneRendererLinearHdrCaptureContext& context)
                            {
                                nvrhi::ITexture* selected = captureSource != FrameCaptureSource::Automatic
                                    ? context.SelectedNativeInput
                                    : (bRawCapture && context.DiagnosticInput ? context.DiagnosticInput : context.LinearHdr);
                                if (captureSource != FrameCaptureSource::Automatic && !selected)
                                {
                                    captureError = std::string("capture_source_unavailable: ") + FrameCaptureSourceName(captureSource);
                                    return;
                                }
                                recordTextureMetadata(selected);
                                impl->RecordingCapture = impl->Readbacks->Record(
                                    context.CommandList,
                                    selected,
                                    TextureReadbackDesc{
                                        .OutputPath = capturePath,
                                        .AlphaMode = bRawCapture ? TextureReadbackAlphaMode::Preserve : TextureReadbackAlphaMode::Discard,
                                        .NonFinitePolicy = TextureReadbackNonFinitePolicy::Preserve,
                                    });
                            })
                        : SceneRendererLinearHdrCaptureRecorder{},
                    .CaptureSource = captureSource,
                    .WorldSnapshot = desc.World,
                });
			bMainSceneRecorded = true;
            if (captureSource != FrameCaptureSource::Automatic && !mImpl->RecordingCapture)
                captureError = std::string("capture_source_unavailable: ") + FrameCaptureSourceName(captureSource);
            Json recordedRasterMetadata;
            if ((!capturePath.empty() || desc.bCaptureSequenceHistory) && desc.World)
                recordedRasterMetadata = mImpl->Scene->GetRecordedRasterCaptureMetadata();
            Json sequenceHistory = nullptr;
            if (desc.bCaptureSequenceHistory)
            {
                sequenceHistory = Json{{"valid", desc.World && !bReuseMainScene && recordedRasterMetadata.value("valid", false)}};
                if (sequenceHistory["valid"].get<bool>())
                {
                    const auto& gi = mImpl->Scene->GetStats().GiStats;
                    sequenceHistory.update(Json{
                        {"worldFrame", desc.World->FrameNumber}, {"cameraCut", desc.World->Camera.bCameraCut},
                        {"historyReset", recordedRasterMetadata.at("historyReset")},
                        {"previousSubmissionId", recordedRasterMetadata.at("previousSubmissionId")},
                        {"jitterPixels", recordedRasterMetadata.at("currentJitterPixels")},
                        {"jitterSequenceGeneration", desc.World->Camera.JitterSequenceGeneration},
                        {"giTechnique", static_cast<uint32_t>(gi.Technique)},
                        {"settingsHash", RenderSettingsService::GetInstance().GetSnapshot().Hash},
                        {"sceneGeneration", desc.World->SceneGeneration},
                        {"viewColumnMajor", recordedRasterMetadata.at("currentViewColumnMajor")},
                        {"giFrameSequenceIndex", gi.FrameSequenceIndex}, {"giHistoryResetCount", gi.HistoryResetCount},
                        {"giLastResetReasons", static_cast<uint32_t>(gi.LastResetReasons)},
                        {"giRaysPerPixel", gi.RaysPerPixel}});
                }
            }
            mImpl->Device.SubmitAndPresent(frame, bVSync);
            if (desc.bCaptureSequenceHistory) sequenceHistory["submission"] = frame.SubmissionInstance;
            publishSubmittedFrame();
			if (!bReuseMainScene)
			{
				mImpl->LastMainWorld = desc.World;
				mImpl->bMainOffscreenContentValid = mImpl->bMainOffscreenEnabled &&
					static_cast<bool>(desc.World);
			}
            mImpl->Readbacks->Collect(frame.CompletedSubmissionInstance);
            if (!capturePath.empty() && desc.World)
            {
                const auto& world = *desc.World;
                const auto vector = [](const glm::vec3& value) {
                    return Json::array({ value.x, value.y, value.z });
                };
                const auto matrix = [](const glm::mat4& value) {
                    Json result = Json::array();
                    for (int column = 0; column < 4; ++column)
                        for (int row = 0; row < 4; ++row)
                            result.push_back(value[column][row]);
                    return result;
                };
                const auto output = mImpl->Scene->GetOffscreenOutput();
                Json snapshot{
                    { "editorView", world.bEditorView },
                    { "frame", world.FrameNumber },
                    { "viewport", { output.Width ? output.Width : swapChain.GetWidth(),
                        output.Height ? output.Height : swapChain.GetHeight() } },
                    { "exposureMultiplier", world.PostProcess.Exposure },
                    { "giDebugView", static_cast<uint32_t>(mImpl->Scene->GetEffectiveGlobalIlluminationSettings().DebugView) },
                    { "giActualRaysPerPixel", mImpl->Scene->GetStats().GiStats.RaysPerPixel },
                    { "giReactiveBudget", {
                        { "enabled", mImpl->Scene->GetStats().GiStats.ReactiveRegion.bEnabled },
                        { "pixels", { mImpl->Scene->GetStats().GiStats.ReactiveRegion.Pixels.x,
                            mImpl->Scene->GetStats().GiStats.ReactiveRegion.Pixels.y,
                            mImpl->Scene->GetStats().GiStats.ReactiveRegion.Pixels.z,
                            mImpl->Scene->GetStats().GiStats.ReactiveRegion.Pixels.w } },
                        { "remainingFrames", mImpl->Scene->GetStats().GiStats.ReactiveRegion.RemainingFrames },
                        { "reasons", mImpl->Scene->GetStats().GiStats.ReactiveRegion.Reasons } } },
                    { "giEnvironmentMisMode", mImpl->Scene->GetStats().GiStats.EnvironmentMisMode },
                    { "giRawAlphaSemantics", mImpl->Scene->GetStats().GiStats.EnvironmentMisMode == 1u
                        ? "proposal-sample-first-hit-fraction" : "cosine-sample-first-hit-fraction" },
                    { "giFrameSequenceIndex", mImpl->Scene->GetStats().GiStats.FrameSequenceIndex },
                    { "captureTexture", capturedTextureMetadata },
                    { "captureError", captureError },
                    { "captureDomain", bRawCapture ? "native-aov" :
                        (bLinearCapture ? "full-resolution-before-tone-map" : "display") },
                    { "environmentIntensity", world.Environment.IblIntensity },
                    { "environmentPath", world.Environment.HdrTexturePath },
                    { "cameraPosition", vector(world.Camera.Position) },
                    { "viewColumnMajor", matrix(world.Camera.View) },
                    { "projectionColumnMajor", matrix(world.Camera.Projection) },
                    { "extractedCameraMatrixProvenance", "RenderWorld input camera; not authoritative jittered raster constants" },
                    { "recordedRaster", recordedRasterMetadata },
                    { "renderWorldFrame", world.FrameNumber },
                    { "applicationSimulationFrame", desc.bApplicationSimulationFrameValid
                        ? Json(desc.ApplicationSimulationFrame) : Json(nullptr) },
                    { "applicationSimulationFrameValid", desc.bApplicationSimulationFrameValid },
                    { "frameIdentityConvention", "frame/renderWorldFrame: RenderSystem extraction sequence; applicationSimulationFrame: SimulationClock at RenderFrame call; graphicsSubmissionId: submitted RenderDevice frame" },
                    { "sceneGeneration", world.SceneGeneration },
                    { "frameSlot", frame.FrameIndex },
                    { "graphicsSubmissionId", frame.SubmissionInstance },
                    { "submissionTimingSampleId", frame.TimingSampleId },
                    { "submissionIssued", frame.bSubmissionIssued },
                    { "objectCount", world.Objects.size() },
                    { "pointLightCount", world.PointLights.size() },
                    { "spotLightCount", world.SpotLights.size() },
                    { "reflectionProbeCount", world.ReflectionProbes.size() },
                    { "bakedIrradianceVolumeCount", world.BakedIrradianceVolumes.size() },
                };
                snapshot["directionalLights"] = Json::array();
                for (const auto& light : world.DirectionalLights)
                    snapshot["directionalLights"].push_back({
                        { "direction", vector(light.Direction) },
                        { "intensity", light.Intensity }, { "color", vector(light.Color) } });
                auto snapshotPath = capturePath;
                snapshotPath += ".world.json";
                if (!snapshotPath.parent_path().empty())
                    std::filesystem::create_directories(snapshotPath.parent_path());
                Utils::WriteTextToFileAtomically(
                    snapshotPath, snapshot.dump(2), Utils::AtomicWriteMode::CreateNew);
            }
            RuntimeRendererFrameResult result{
                .FrameSlot = frame.FrameIndex,
                .Submission = frame.SubmissionInstance,
                .Capture = mImpl->RecordingCapture,
                .CaptureError = captureError,
                .SequenceHistory = std::move(sequenceHistory),
            };
            mImpl->RecordingCapture = {};
            mImpl->ActiveSimulationFrame = nullptr;
            return result;
        }
        catch (...)
        {
            const std::exception_ptr failure = std::current_exception();
            mImpl->ActiveSimulationFrame = nullptr;
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
                EffekseerRuntime::AbortHostFrame(frame.FrameIndex);
				mImpl->Device.AbortFrame(frame);
				if (bSceneOverlayRecorded && overlayRecorder)
					overlayRecorder->AbortSceneOverlayFrame(frame.FrameIndex);
				if (bMainSceneRecorded)
					mImpl->Scene->AbortFrame(frame.FrameIndex);
				for (const uint32_t index : recordedOffscreenViews)
				{
					mImpl->OffscreenViews[index].Renderer->AbortFrame(frame.FrameIndex);
                    mImpl->OffscreenViews[index].bSubmitted = static_cast<bool>(mImpl->OffscreenViews[index].World);
                }
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
                else
                    mImpl->Readbacks->Release(mImpl->RecordingCapture);
                mImpl->RecordingCapture = {};
            }
            std::rethrow_exception(failure);
        }
    }

    void RuntimeRenderer::Resize(uint32_t width, uint32_t height)
    {
        if (!IsInitialized() || width == 0 || height == 0)
            return;
        if (mImpl->ActiveSimulationFrame)
        {
            mImpl->PendingWidth = width;
            mImpl->PendingHeight = height;
            return;
        }
        mImpl->Device.WaitForIdle();
        // Window resize events report client coordinates. The native swap chain
        // resolves the actual framebuffer extent (for example 2x on Retina).
        // Resize scene resources from those pixels, as Initialize does, and do
        // not compare the logical request against the previous pixel extent.
        mImpl->Device.Resize(width, height);
        const auto renderWidth = mImpl->Device.GetSwapChain().GetWidth();
        const auto renderHeight = mImpl->Device.GetSwapChain().GetHeight();
		mImpl->bMainOffscreenContentValid = false;
        mImpl->Scene->Resize(renderWidth, renderHeight);
        mImpl->ViewportWidth = renderWidth;
        mImpl->ViewportHeight = renderHeight;
        mImpl->RejectedAutomaticInputs.reset();
		if (mImpl->bMainOffscreenEnabled)
		{
			mImpl->Scene->ConfigureOffscreenOutput(
				renderWidth,
				renderHeight,
				"Runtime Main Scene Viewport", mImpl->Device.GetSwapChain().GetFormat());
		}
        mImpl->Readbacks->Collect(mImpl->LastSubmission);
    }

    void RuntimeRenderer::WaitForIdle()
    {
        if (!IsInitialized())
            return;
        mImpl->RequireFrameBoundary("WaitForIdle");
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
        if (mImpl->ActiveSimulationFrame)
        {
            mImpl->BoundaryRequests.emplace_back("AA configuration", [this, settings] { SetAntiAliasingSettings(settings); });
            return;
        }
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
            throw std::logic_error("RuntimeRenderer settings transaction requires Initialize");
        if (mImpl->ActiveSimulationFrame)
        {
            mImpl->BoundaryRequests.emplace_back("Render settings transaction", [this, settings, plan] { ApplyRenderSettingsBatch(settings, plan); });
            return;
        }
        if (settings.Effective.FramesInFlight != GetFramesInFlight())
            throw std::runtime_error("Changing Rendering.Presentation.FramesInFlight requires process restart");

        SceneRendererDesc target = mImpl->SceneDesc;
        target.PipelineType = settings.Effective.PipelineType;
        target.PipelineFeatures = settings.Effective.PipelineFeatures;
        target.TransientCacheBudgetBytes = settings.Effective.TransientCacheBudgetMiB == 0
            ? std::numeric_limits<uint64_t>::max()
            : uint64_t(settings.Effective.TransientCacheBudgetMiB) * 1024u * 1024u;
        target.Path = settings.Effective.Path;
        target.AntiAliasing = settings.Requested.AntiAliasing;
        target.Upscaling = settings.Effective.Upscaling;
        target.TextureStreamingBudgetBytes = uint64_t(settings.Effective.TextureStreamingBudgetMiB) * 1024 * 1024;
        target.AntiAliasing.CaptureSupersampleScale = settings.Capture.SupersampleScale;
        target.GlobalIllumination = settings.Requested.GlobalIllumination;
        target.GlobalIllumination.DebugView = settings.Debug.GiDebugView;
        target.GlobalIllumination.bDirectOnly = settings.Debug.bGiDirectOnly;
        target.AmbientOcclusion = settings.Effective.AmbientOcclusion;
        target.Clouds = settings.Effective.Clouds;
        target.Reflections = settings.Effective.Reflections;
        target.PostProcess = settings.Effective.PostProcess;
        target.bGpuOcclusionEnabled = settings.Effective.bGpuOcclusionEnabled;
        target.bGpuBatchingEnabled = settings.Effective.bGpuBatchingEnabled;
        target.bGpuTightFrustumEnabled = settings.Effective.bGpuTightFrustumEnabled;
        target.bGpuShadowBatchingEnabled = settings.Effective.bGpuShadowBatchingEnabled;
        target.bTransientTextureReuse = settings.Effective.bTransientTextureReuse;
        target.bLodEnabled = settings.Effective.bLodEnabled;
        target.bShadowsEnabled = settings.Effective.Shadows.bEnabled;
        target.bContinuousPcfEnabled = settings.Effective.Shadows.bContinuousPcfEnabled;
        target.LodMaxPixelError = settings.Effective.LodMaxPixelError;
        mImpl->ApplyViewConfigurations(mImpl->LastMainWorld ? *mImpl->LastMainWorld : mImpl->EmptyWorld,
            settings, target);
        mImpl->SceneDesc = std::move(target);
        mImpl->BaseSettings = settings;
        mImpl->RejectedAutomaticInputs.reset();

        const bool bPbrDebugChanged = std::ranges::find(plan.ChangedPropertyIds,
            RenderSettingIds::PbrDebugMode) != plan.ChangedPropertyIds.end();
        const bool bShadowDebugChanged = std::ranges::find(plan.ChangedPropertyIds,
            RenderSettingIds::ShadowCascadeDebug) != plan.ChangedPropertyIds.end();
        if (bPbrDebugChanged || bShadowDebugChanged)
        {
            const auto applyDebug = [&](RenderPipeline& renderer)
            {
                if (bPbrDebugChanged)
                    renderer.SetPbrDebugMode(static_cast<ScenePbrDebugMode>(settings.Debug.PbrDebugMode));
                if (bShadowDebugChanged)
                    renderer.SetDirectionalShadowCascadeDebug(settings.Debug.bShadowCascadeDebug);
            };
            applyDebug(*mImpl->Scene);
            for (Impl::OffscreenView& view : mImpl->OffscreenViews)
                if (view.bLive && view.Renderer) applyDebug(*view.Renderer);
        }
    }

    RuntimeConfigurationPreparationStats RuntimeRenderer::GetConfigurationPreparationStats() const noexcept
    {
        return mImpl ? mImpl->ConfigurationPreparationStats : RuntimeConfigurationPreparationStats{};
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
        if (mImpl->ActiveSimulationFrame)
        {
            mImpl->BoundaryRequests.emplace_back("GI configuration", [this, settings] { SetGlobalIlluminationSettings(settings); });
            return;
        }
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
        if (mImpl->ActiveSimulationFrame)
        {
            mImpl->BoundaryRequests.emplace_back("GI history reset", [this] { ResetGlobalIlluminationHistory(); });
            return;
        }
        mImpl->Device.WaitForIdle();
        mImpl->Scene->ResetGlobalIlluminationHistory();
        for (Impl::OffscreenView& view : mImpl->OffscreenViews)
        {
            if (view.bLive && view.Renderer)
                view.Renderer->ResetGlobalIlluminationHistory();
        }
        mImpl->bMainOffscreenContentValid = false;
    }

    void RuntimeRenderer::ResetTemporalHistory()
    {
        if (!IsInitialized()) return;
        // Only invalidation flags and sample counters change. Existing frame-slot
        // fences continue to protect history textures when the next frame records.
        mImpl->Scene->ResetTemporalHistory();
        for (Impl::OffscreenView& view : mImpl->OffscreenViews)
            if (view.bLive && view.Renderer)
                view.Renderer->ResetTemporalHistory();
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
        mImpl->Scene->ActivateOfflineViewportRenderer(std::move(providerId));
        mImpl->bMainOffscreenContentValid = false;
    }

    void RuntimeRenderer::DeactivateOfflineViewportRenderer()
    {
        if (!IsInitialized())
            return;
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

#if defined(HE_DEV_AGENT_RUNTIME)
    bool RuntimeRenderer::InjectUpscalingFailureForValidation(bool duringDispatch)
    {
        return IsInitialized() && mImpl->Scene->InjectUpscalingFailureForValidation(duringDispatch);
    }
#endif
	size_t RuntimeRenderer::RequestTextureReload(std::string_view assetPath)
	{
		if (!IsInitialized())
			throw std::logic_error("RuntimeRenderer texture reload requires Initialize");
		return mImpl->Scene->RequestTextureReload(assetPath);
	}

	void RuntimeRenderer::EnableMainOffscreenOutput()
	{
		if (!IsInitialized())
			throw std::logic_error("RuntimeRenderer main offscreen output requires Initialize");
		const uint32_t width = GetWidth();
		const uint32_t height = GetHeight();
		const auto colorFormat = mImpl->Device.GetSwapChain().GetFormat();
		const SceneRendererOffscreenOutput current = mImpl->Scene->GetOffscreenOutput();
		if (mImpl->bMainOffscreenEnabled && current &&
			current.Width == width && current.Height == height && current.ColorFormat == colorFormat)
		{
			return;
		}
		if (mImpl->ActiveSimulationFrame)
		{
			mImpl->BoundaryRequests.emplace_back("Main offscreen output configuration", [this] { EnableMainOffscreenOutput(); });
			return;
		}
		mImpl->Device.WaitForIdle();
		mImpl->Scene->ConfigureOffscreenOutput(width, height, "Runtime Main Scene Viewport", colorFormat);
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
            sceneDesc.bEnableCpuGraphTiming = false;
			view.Renderer = CreateRenderPipeline(sceneDesc);
			view.Renderer->ConfigureOffscreenOutput(width, height, view.DebugName);
			view.bLive = true;
			return { index, view.Generation };
		}
		catch (...)
		{
			view.Renderer.reset();
            view.PreviousOutput = {};
            view.bReplacementOutputPending = false;
            view.OutputGenerationOffset = 0;
            view.PendingOutputGenerationOffset = 0;
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
		if (mImpl->ActiveSimulationFrame)
		{
			view.PendingWidth = width;
			view.PendingHeight = height;
			return;
		}
		const SceneRendererOffscreenOutput current = view.Renderer->GetOffscreenOutput();
		if (current.Width == width && current.Height == height)
			return;
		mImpl->Device.WaitForIdle();
		view.Renderer->ConfigureOffscreenOutput(width, height, view.DebugName);
        mImpl->RejectedAutomaticInputs.reset();
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
			view.Renderer->PrepareReflectionProbeResources(*world);
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
        auto output = ToRuntimeOffscreenOutput(view.bReplacementOutputPending
            ? view.PreviousOutput : view.Renderer->GetOffscreenOutput());
        output.Generation += view.OutputGenerationOffset;
        return output;
	}

	Ref<const RenderWorld> RuntimeRenderer::GetLastSubmittedRenderWorld() const noexcept
	{
		// LastMainWorld is published only after SubmitAndPresent succeeds.
		return mImpl ? mImpl->LastMainWorld : Ref<const RenderWorld>{};
	}

	const SceneRendererStats& RuntimeRenderer::GetOffscreenSceneStats(RuntimeOffscreenViewHandle handle) const
	{
		if (!IsInitialized() || !handle || handle.Index >= mImpl->OffscreenViews.size())
			throw std::out_of_range("Invalid runtime offscreen view handle");
		const auto& view = mImpl->OffscreenViews[handle.Index];
		if (!view.bLive || view.Generation != handle.Generation || !view.Renderer)
			throw std::out_of_range("Stale runtime offscreen view handle");
		return view.Renderer->GetStats();
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
		if (mImpl->ActiveSimulationFrame)
		{
			mImpl->BoundaryRequests.emplace_back("Offscreen view destruction", [this, handle] { DestroyOffscreenView(handle); });
			return;
		}
		if (view.PendingCapture && mImpl->Readbacks->IsValid(view.PendingCapture))
			mImpl->Readbacks->CancelPending(view.PendingCapture);
		view.PendingCapture = {};
		mImpl->Device.WaitForIdle();
		view.PendingWidth = view.PendingHeight = 0;
		view.Renderer.reset();
        view.PreviousOutput = {};
        view.bReplacementOutputPending = false;
        view.OutputGenerationOffset = 0;
        view.PendingOutputGenerationOffset = 0;
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

    bool RuntimeRenderer::IsCaptureSourceAvailable(FrameCaptureSource source) const
    {
        return IsInitialized() && mImpl->Scene->IsCaptureSourceAvailable(source);
    }

    uint32_t RuntimeRenderer::GetFramesInFlight() const
    {
        if (!IsInitialized())
            throw std::logic_error("RuntimeRenderer capabilities query requires Initialize");
        return mImpl->Device.GetCapabilities().FramesInFlight;
    }

    void RuntimeRenderer::BeginGpuTimingFrame(uint64_t cpuFrameId)
    {
        if (IsInitialized()) mImpl->Device.BeginGpuTimingFrame(cpuFrameId);
    }

    void RuntimeRenderer::CancelGpuTimingFrame() noexcept
    {
        if (IsInitialized()) mImpl->Device.CancelGpuTimingFrame();
    }

    std::vector<RenderSubmissionGpuTiming> RuntimeRenderer::TakeSubmissionTimings()
    {
        return IsInitialized() ? mImpl->Device.TakeSubmissionTimings()
            : std::vector<RenderSubmissionGpuTiming>{};
    }

    uint64_t RuntimeRenderer::GetLastTimingSampleId() const
    {
        return IsInitialized() ? mImpl->Device.GetLastTimingSampleId() : 0;
    }

    RenderMemoryBudget RuntimeRenderer::QueryMemoryBudget() const
    {
        return IsInitialized() ? mImpl->Device.QueryMemoryBudget() : RenderMemoryBudget{};
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

    const SceneRendererStats& RuntimeRenderer::GetSceneStats() const
    {
        if (!IsInitialized())
            throw std::logic_error("Scene statistics require an initialized renderer");
        return mImpl->Scene->GetStats();
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
