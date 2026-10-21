#include "Hepch.h"
#include "SceneUpscalingPass.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/Scene/SubmittedSkyMotionHistory.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <tuple>

namespace HEngine
{
    struct SceneUpscalingPass::Impl
    {
        struct State
        {
            std::unique_ptr<ITemporalUpscaler> Upscaler;
            TemporalUpscalerFrame Frame;
            RenderExtent Render{}, Output{};
            uint32_t Sequence = 0;
            bool bReset = true;
            bool bRecorded = false;
            bool bDispatchReady = false;
            SubmittedSkyMotionHistory SkyHistory;
            glm::mat4 SkyReprojection{1.0f};
            double LastTime = -1.0;
            uint64_t ProjectionGeneration = 0;
#if defined(HE_DEV_AGENT_RUNTIME)
            uint8_t PendingValidationFailure = 0;
#endif
        };
        struct PassData
        {
            TextureHandle Color, Opaque, Depth, Motion, MotionCopy, DepthCopy, Reactive, Composition, Output;
        };
        struct Constants { uint32_t Width, Height, Padding[2]{}; };
        struct InputConstants { uint32_t Width, Height, Padding[2]{}; glm::mat4 SkyReprojection{1.0f}; };
        static_assert(sizeof(InputConstants) == 80);
        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaders;
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle Layout;
        nvrhi::ComputePipelineHandle Pipeline;
        nvrhi::ShaderHandle FallbackShader;
        nvrhi::BindingLayoutHandle FallbackLayout;
        nvrhi::ComputePipelineHandle FallbackPipeline;
        nvrhi::ShaderHandle CoverageShader;
        nvrhi::BindingLayoutHandle CoverageLayout;
        nvrhi::ComputePipelineHandle CoveragePipeline;
        nvrhi::SamplerHandle LinearClamp;
        std::shared_ptr<State> Active;
        std::vector<std::pair<uint64_t, std::shared_ptr<State>>> Retired;
        uint64_t LastSubmitted = 0;
        UpscalingSettings Settings;
        std::string Reason;
        bool bFailed = false;
        bool bEligible = false;
        bool bHasOutput = false;
        uint64_t FallbackFramesRecorded = 0;
        TextureHandle ReactiveMask, CompositionMask, MotionVectors, InputColor, OpaqueColor;

        explicit Impl(const SceneAntiAliasingPassDesc& desc) : Device(desc.Device)
        {
            if (!desc.SharedShaderManager) OwnedShaders = CreateScope<ShaderManager>(Device);
            auto& shaders = desc.SharedShaderManager ? *desc.SharedShaderManager : *OwnedShaders;
            const auto format = GetShaderBinaryFormat(desc.Backend);
            const auto path = desc.ShaderArtifactCacheRoot / std::string(GetShaderArtifactDirectoryName(format)) / "SceneUpscalingInputs.cs";
            std::ifstream keys(MakeShaderCacheKeyPath(path));
            std::string hash;
            for (std::string line; std::getline(keys, line);)
                if (line.starts_with("key=")) { hash = line.substr(4); break; }
            if (hash.empty()) throw std::runtime_error("FSR input shader cache key is missing");
            Shader = shaders.LoadShaderFile(ShaderCacheKey{
                .Source = "Modern/SceneUpscalingInputs.hlsl", .Variant = "BASE", .Stage = nvrhi::ShaderType::Compute,
                .EntryPoint = "CS", .TargetProfile = "cs_6_0", .ContentHash = hash,
                .Backend = desc.Backend, .BinaryFormat = format}, path, "FSR inputs CS");
            nvrhi::BindingLayoutDesc layout;
            layout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(InputConstants)));
            for (uint32_t index = 0; index < 4; ++index)
                layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(index));
            for (uint32_t index = 0; index < 4; ++index)
                layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(index));
            Layout = Device->createBindingLayout(layout);
            Pipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(Shader).addBindingLayout(Layout));
            if (!Layout || !Pipeline) throw std::runtime_error("FSR inputs pipeline creation failed");
            const auto fallbackPath = path.parent_path() / "SceneUpscalingFallback.cs";
            std::ifstream fallbackKeys(MakeShaderCacheKeyPath(fallbackPath));
            hash.clear();
            for (std::string line; std::getline(fallbackKeys, line);)
                if (line.starts_with("key=")) { hash = line.substr(4); break; }
            if (hash.empty()) throw std::runtime_error("FSR fallback shader cache key is missing");
            FallbackShader = shaders.LoadShaderFile(ShaderCacheKey{
                .Source = "Modern/SceneUpscalingFallback.hlsl", .Variant = "BASE", .Stage = nvrhi::ShaderType::Compute,
                .EntryPoint = "CS", .TargetProfile = "cs_6_0", .ContentHash = hash,
                .Backend = desc.Backend, .BinaryFormat = format}, fallbackPath, "FSR failure fallback CS");
            nvrhi::BindingLayoutDesc fallbackLayout;
            fallbackLayout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            FallbackLayout = Device->createBindingLayout(fallbackLayout);
            FallbackPipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
                .setComputeShader(FallbackShader).addBindingLayout(FallbackLayout));
            LinearClamp = Device->createSampler(nvrhi::SamplerDesc()
                .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setAllFilters(true));
            if (!FallbackLayout || !FallbackPipeline || !LinearClamp)
                throw std::runtime_error("FSR failure fallback pipeline creation failed");
            const auto coveragePath = path.parent_path() / "SceneUpscalingCoverage.cs";
            std::ifstream coverageKeys(MakeShaderCacheKeyPath(coveragePath));
            hash.clear();
            for (std::string line; std::getline(coverageKeys, line);)
                if (line.starts_with("key=")) { hash = line.substr(4); break; }
            if (hash.empty()) throw std::runtime_error("FSR coverage shader cache key is missing");
            CoverageShader = shaders.LoadShaderFile(ShaderCacheKey{
                .Source = "Modern/SceneUpscalingCoverage.hlsl", .Variant = "BASE", .Stage = nvrhi::ShaderType::Compute,
                .EntryPoint = "CS", .TargetProfile = "cs_6_0", .ContentHash = hash,
                .Backend = desc.Backend, .BinaryFormat = format}, coveragePath, "FSR coverage initialization CS");
            nvrhi::BindingLayoutDesc coverageLayout;
            coverageLayout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
            CoverageLayout = Device->createBindingLayout(coverageLayout);
            CoveragePipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
                .setComputeShader(CoverageShader).addBindingLayout(CoverageLayout));
            if (!CoverageLayout || !CoveragePipeline)
                throw std::runtime_error("FSR coverage initialization pipeline creation failed");
        }
    };

    SceneUpscalingPass::SceneUpscalingPass(const SceneAntiAliasingPassDesc& desc) : mImpl(CreateScope<Impl>(desc)) {}
    SceneUpscalingPass::~SceneUpscalingPass() = default;
    bool SceneUpscalingPass::IsActive() const noexcept { return mImpl->Active != nullptr && !mImpl->bFailed; }
    bool SceneUpscalingPass::HasFailed() const noexcept { return mImpl->bFailed; }
    UpscalingStatus SceneUpscalingPass::GetStatus() const noexcept
    {
        if (mImpl->Settings.Mode == UpscalingMode::Off) return UpscalingStatus::Off;
        if (!mImpl->bEligible) return UpscalingStatus::UnsupportedView;
        if (!mImpl->bHasOutput) return UpscalingStatus::Suspended;
        if (mImpl->bFailed) return UpscalingStatus::Failed;
        return IsActive() ? UpscalingStatus::Active : UpscalingStatus::Pending;
    }
    const std::string& SceneUpscalingPass::GetReason() const noexcept { return mImpl->Reason; }

    RenderExtent SceneUpscalingPass::Prepare(const UpscalingSettings& settings, RenderExtent output,
        RenderExtent fallback, bool eligible, uint64_t completed)
    {
        auto& impl = *mImpl;
        std::erase_if(impl.Retired, [completed](const auto& item) { return item.first <= completed; });
        if (settings.Mode != impl.Settings.Mode || settings.Quality != impl.Settings.Quality) impl.bFailed = false;
        if (!impl.bFailed) impl.Reason.clear();
        impl.Settings = settings;
        impl.bEligible = eligible;
        impl.bHasOutput = output.Width != 0 && output.Height != 0;
        const bool requested = settings.Mode == UpscalingMode::Fsr31 && eligible && output.Width && output.Height;
        const auto render = requested ? ResolveUpscalingExtent(output.Width, output.Height, settings.Quality) : fallback;
        if (impl.Active && (!requested || impl.bFailed || impl.Active->Render.Width != render.Width ||
            impl.Active->Render.Height != render.Height || impl.Active->Output.Width != output.Width || impl.Active->Output.Height != output.Height))
        {
            impl.Retired.emplace_back(impl.LastSubmitted, std::move(impl.Active));
        }
        if (!requested || impl.bFailed) return fallback;
        if (!impl.Active)
        {
            auto state = std::make_shared<Impl::State>();
            state->Upscaler = CreateFsr31Upscaler(impl.Device,
                TemporalUpscalerDesc{.RenderWidth = render.Width, .RenderHeight = render.Height,
                    .OutputWidth = output.Width, .OutputHeight = output.Height,
                    .bJitteredMotionVectors = true}, impl.Reason);
            if (!state->Upscaler)
            {
                impl.bFailed = true;
                HE_CORE_WARN("FSR fallback: {}", impl.Reason);
                return fallback;
            }
            state->Render = render; state->Output = output;
            impl.Active = std::move(state);
            HE_CORE_INFO("FSR {} active: {}x{} -> {}x{}", impl.Active->Upscaler->GetVersion(),
                render.Width, render.Height, output.Width, output.Height);
        }
        impl.Active->Frame.Sharpness = settings.Sharpness;
        return render;
    }

    glm::vec2 SceneUpscalingPass::PrepareFrame(const RenderCamera& camera, double timeSeconds, bool reset)
    {
        if (!IsActive()) return {};
        auto& state = *mImpl->Active;
        reset |= state.ProjectionGeneration != camera.ProjectionGeneration;
        state.ProjectionGeneration = camera.ProjectionGeneration;
        if (reset) { state.bReset = true; state.Sequence = 0; }
        auto& frame = state.Frame;
        frame.bReset = state.bReset;
        frame.RenderWidth = state.Render.Width; frame.RenderHeight = state.Render.Height;
        frame.MotionScaleX = float(state.Render.Width); frame.MotionScaleY = float(state.Render.Height);
        frame.CameraNear = camera.NearPlane; frame.CameraFar = camera.FarPlane;
        frame.VerticalFovRadians = 2.0f * std::atan(1.0f / std::abs(camera.Projection[1][1]));
        frame.FrameTimeMilliseconds = state.LastTime < 0.0 ? 16.667f : float(std::clamp(timeSeconds - state.LastTime, 0.0, 1.0) * 1000.0);
        state.LastTime = timeSeconds;
        // HDR input has not had exposure applied yet. FSR computes its internal
        // luminance exposure; the scene's artistic/auto exposure follows FSR.
        frame.PreExposure = 1.0f;
        state.bDispatchReady = state.Upscaler->GetJitter(
            state.Sequence, state.Render.Width, frame.JitterX, frame.JitterY, mImpl->Reason);
#if defined(HE_DEV_AGENT_RUNTIME)
        if (state.PendingValidationFailure == 1)
        {
            state.PendingValidationFailure = 0;
            state.bDispatchReady = false;
            mImpl->Reason = "FSR validation: injected jitter query failure";
        }
#endif
        if (!state.bDispatchReady)
        {
            // Keep this frame's graph and render extent consistent. Its upscale
            // pass writes spatial fallback; Prepare retires FSR on the next frame.
            frame.JitterX = frame.JitterY = 0.0f;
        }
        // Match the SDK FSR API sample: negate the queried Halton offset for
        // both projection translation (-2*x, +2*y in NDC) and dispatch jitter.
        frame.JitterX = -frame.JitterX;
        frame.JitterY = -frame.JitterY;
        state.SkyReprojection = state.SkyHistory.Stage(camera.View, camera.Projection,
            {frame.JitterX, frame.JitterY}, {float(state.Render.Width), float(state.Render.Height)}, frame.bReset);
        return {frame.JitterX, frame.JitterY};
    }

    void SceneUpscalingPass::BeginTransparency(RenderGraph& graph, TextureHandle color)
    {
        if (!IsActive()) return;
        const auto state = mImpl->Active;
        graph.AddPass<TextureHandle>("FSRBeginTransparency", RenderGraphQueue::Compute,
            [=](RenderGraphBuilder& builder, TextureHandle& data) {
                data = builder.ReadWrite(color, RenderGraphUsage::UnorderedAccess);
            },
            [impl = mImpl.get(), state](RenderGraphContext& context, const TextureHandle& data) {
                nvrhi::BindingSetDesc bindings;
                bindings.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Impl::Constants)))
                    .addItem(nvrhi::BindingSetItem::Texture_UAV(0, context.GetTexture(data, RenderGraphAccess::ReadWrite)));
                const auto binding = impl->Device->createBindingSet(bindings, impl->CoverageLayout);
                auto* commands = context.GetCommandList();
                commands->setComputeState(nvrhi::ComputeState().setPipeline(impl->CoveragePipeline).addBindingSet(binding));
                const Impl::Constants constants{state->Render.Width, state->Render.Height};
                commands->setPushConstants(&constants, sizeof(constants));
                commands->dispatch((constants.Width + 7) / 8, (constants.Height + 7) / 8);
            });
    }

    TextureHandle SceneUpscalingPass::AddPasses(RenderGraph& graph, TextureHandle color,
        TextureHandle opaque, TextureHandle depth, TextureHandle motion)
    {
        if (!IsActive()) return color;
        mImpl->InputColor = color; mImpl->OpaqueColor = opaque;
        const auto state = mImpl->Active;
        auto create = [&](const char* name, nvrhi::Format format, RenderExtent size) {
            return graph.CreateTexture(RenderGraphTextureDesc{.DebugName = name, .Width = size.Width,
                .Height = size.Height, .Format = format});
        };
        const auto depthCopy = create("FSR Device Depth", nvrhi::Format::R32_FLOAT, state->Render);
        const auto motionCopy = create("FSR Motion Vectors", nvrhi::Format::RG16_FLOAT, state->Render);
        mImpl->MotionVectors = motionCopy;
        const auto reactive = create("FSR Reactive Mask", nvrhi::Format::R8_UNORM, state->Render);
        const auto composition = create("FSR Composition Mask", nvrhi::Format::R8_UNORM, state->Render);
        mImpl->ReactiveMask = reactive; mImpl->CompositionMask = composition;
        const auto output = create("FSR Reconstructed HDR", nvrhi::Format::RGBA16_FLOAT, state->Output);
        graph.AddPass<Impl::PassData>("FSRPrepareInputs", RenderGraphQueue::Compute,
            [=](RenderGraphBuilder& builder, Impl::PassData& data) {
                data.Color = builder.ShaderResource(color); data.Opaque = builder.ShaderResource(opaque);
                data.Depth = builder.ShaderResource(depth);
                data.Motion = builder.ShaderResource(motion);
                data.MotionCopy = builder.Write(motionCopy, RenderGraphUsage::UnorderedAccess);
                data.DepthCopy = builder.Write(depthCopy, RenderGraphUsage::UnorderedAccess);
                data.Reactive = builder.Write(reactive, RenderGraphUsage::UnorderedAccess);
                data.Composition = builder.Write(composition, RenderGraphUsage::UnorderedAccess);
            },
            [impl = mImpl.get(), state](RenderGraphContext& context, const Impl::PassData& data) {
                nvrhi::BindingSetDesc bindings;
                bindings.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Impl::InputConstants)))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(0, context.GetTexture(data.Color, RenderGraphAccess::Read)))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(1, context.GetTexture(data.Opaque, RenderGraphAccess::Read)))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(2, context.GetTexture(data.Depth, RenderGraphAccess::Read)))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(3, context.GetTexture(data.Motion, RenderGraphAccess::Read)))
                    .addItem(nvrhi::BindingSetItem::Texture_UAV(0, context.GetTexture(data.DepthCopy, RenderGraphAccess::Write)))
                    .addItem(nvrhi::BindingSetItem::Texture_UAV(1, context.GetTexture(data.Reactive, RenderGraphAccess::Write)))
                    .addItem(nvrhi::BindingSetItem::Texture_UAV(2, context.GetTexture(data.Composition, RenderGraphAccess::Write)))
                    .addItem(nvrhi::BindingSetItem::Texture_UAV(3, context.GetTexture(data.MotionCopy, RenderGraphAccess::Write)));
                auto binding = impl->Device->createBindingSet(bindings, impl->Layout);
                auto* commands = context.GetCommandList();
                commands->setComputeState(nvrhi::ComputeState().setPipeline(impl->Pipeline).addBindingSet(binding));
                const Impl::InputConstants constants{state->Render.Width, state->Render.Height, {}, state->SkyReprojection};
                commands->setPushConstants(&constants, sizeof(constants));
                commands->dispatch((constants.Width + 7) / 8, (constants.Height + 7) / 8);
            });
        graph.AddPass<Impl::PassData>("FSR31Upscale", RenderGraphQueue::Compute,
            [=](RenderGraphBuilder& builder, Impl::PassData& data) {
                data.Color = builder.ShaderResource(color); data.DepthCopy = builder.ShaderResource(depthCopy);
                data.Motion = builder.ShaderResource(motionCopy); data.Reactive = builder.ShaderResource(reactive);
                data.Composition = builder.ShaderResource(composition);
                data.Output = builder.Write(output, RenderGraphUsage::UnorderedAccess);
            },
            [impl = mImpl.get(), state](RenderGraphContext& context, const Impl::PassData& data) {
                auto frame = state->Frame;
                frame.CommandList = context.GetCommandList();
                frame.Color = context.GetTexture(data.Color, RenderGraphAccess::Read);
                frame.Depth = context.GetTexture(data.DepthCopy, RenderGraphAccess::Read);
                frame.MotionVectors = context.GetTexture(data.Motion, RenderGraphAccess::Read);
                frame.Reactive = context.GetTexture(data.Reactive, RenderGraphAccess::Read);
                frame.Composition = context.GetTexture(data.Composition, RenderGraphAccess::Read);
                frame.Output = context.GetTexture(data.Output, RenderGraphAccess::Write);
                bool bDispatchReady = state->bDispatchReady;
#if defined(HE_DEV_AGENT_RUNTIME)
                if (state->PendingValidationFailure == 2)
                {
                    state->PendingValidationFailure = 0;
                    bDispatchReady = false;
                    impl->Reason = "FSR validation: injected dispatch failure";
                }
#endif
                if (!bDispatchReady || !state->Upscaler->Dispatch(frame, impl->Reason))
                {
                    impl->bFailed = true;
                    HE_CORE_ERROR("{}; spatial fallback this frame, native AA next frame", impl->Reason);
                    nvrhi::BindingSetDesc bindings;
                    bindings.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Impl::Constants)))
                        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, frame.Color))
                        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, frame.Output))
                        .addItem(nvrhi::BindingSetItem::Sampler(0, impl->LinearClamp));
                    auto binding = impl->Device->createBindingSet(bindings, impl->FallbackLayout);
                    frame.CommandList->setComputeState(nvrhi::ComputeState().setPipeline(impl->FallbackPipeline).addBindingSet(binding));
                    const Impl::Constants constants{state->Output.Width, state->Output.Height};
                    frame.CommandList->setPushConstants(&constants, sizeof(constants));
                    frame.CommandList->dispatch((constants.Width + 7) / 8, (constants.Height + 7) / 8);
                    ++impl->FallbackFramesRecorded;
                    state->bReset = true;
                    return;
                }
                state->bRecorded = true;
            });
        return output;
    }

    void SceneUpscalingPass::ResetHistory() noexcept
    {
        if (mImpl->Active)
        {
            mImpl->Active->bReset = true; mImpl->Active->Frame.bReset = true; mImpl->Active->Sequence = 0;
            // Material publication can reset after Stage. Preserve that pending
            // camera so successful submission still seeds the following frame.
            mImpl->Active->SkyReprojection = glm::mat4(1.0f);
        }
    }
    void SceneUpscalingPass::NotifySubmitted(uint64_t submission) noexcept
    {
        mImpl->LastSubmitted = submission;
        if (mImpl->Active && mImpl->Active->bRecorded)
        {
            mImpl->Active->bRecorded = false; mImpl->Active->bReset = false; ++mImpl->Active->Sequence;
            mImpl->Active->SkyHistory.Commit();
        }
    }
    void SceneUpscalingPass::AbortFrame() noexcept
    { if (mImpl->Active) { mImpl->Active->bRecorded = false; ResetHistory(); } }
    void SceneUpscalingPass::Clear() { mImpl->Active.reset(); mImpl->Retired.clear(); }
    TextureHandle SceneUpscalingPass::GetReactiveMask() const noexcept
    { return IsActive() ? mImpl->ReactiveMask : TextureHandle{}; }
    TextureHandle SceneUpscalingPass::GetCompositionMask() const noexcept
    { return IsActive() ? mImpl->CompositionMask : TextureHandle{}; }
    TextureHandle SceneUpscalingPass::GetMotionVectors() const noexcept
    { return IsActive() ? mImpl->MotionVectors : TextureHandle{}; }
    TextureHandle SceneUpscalingPass::GetInputColor() const noexcept
    { return IsActive() ? mImpl->InputColor : TextureHandle{}; }
    TextureHandle SceneUpscalingPass::GetOpaqueColor() const noexcept
    { return IsActive() ? mImpl->OpaqueColor : TextureHandle{}; }
#if defined(HE_DEV_AGENT_RUNTIME)
    bool SceneUpscalingPass::InjectFailureForValidation(bool duringDispatch) noexcept
    {
        if (!IsActive() || mImpl->Active->PendingValidationFailure) return false;
        mImpl->Active->PendingValidationFailure = duringDispatch ? 2 : 1;
        return true;
    }
#endif
    Json SceneUpscalingPass::GetDiagnostics() const
    {
        Json result{{"active", IsActive()}, {"mode", ToString(mImpl->Settings.Mode)},
            {"status", ToString(GetStatus())},
            {"quality", ToString(mImpl->Settings.Quality)}, {"sharpness", mImpl->Settings.Sharpness},
            {"failure", mImpl->bFailed}, {"reason", mImpl->Reason},
            {"fallbackFramesRecorded", mImpl->FallbackFramesRecorded}};
        if (const auto& state = mImpl->Active)
        {
            result["provider"] = state->Upscaler->GetVersion();
            result["renderSize"] = {state->Render.Width, state->Render.Height};
            result["outputSize"] = {state->Output.Width, state->Output.Height};
            result["jitter"] = {state->Frame.JitterX, state->Frame.JitterY};
            result["reset"] = state->Frame.bReset;
            result["submittedFrames"] = state->Sequence;
            result["preExposure"] = state->Frame.PreExposure;
            result["dispatchReady"] = state->bDispatchReady;
        }
        return result;
    }
}
