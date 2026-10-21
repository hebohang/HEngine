#include "Hepch.h"

#include "Runtime/Graphics/Renderer/ScenePostProcessPass.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    namespace
    {
        enum Operation : uint32_t { Downsample, BlurHorizontal, BlurVertical, LuminanceTiles, AdaptExposure, Composite };

        struct alignas(16) Constants
        {
            glm::uvec2 SourceDimensions{ 1u };
            glm::uvec2 OutputDimensions{ 1u };
            uint32_t Op = 0;
            uint32_t AutoExposure = 0;
            uint32_t ResetHistory = 1;
            uint32_t Padding = 0;
            glm::vec4 BloomExposure{ 0.05f, 1.0f, 0.8f, 0.18f };
            glm::vec4 Adaptation{ 0.05f, 16.0f, 2.0f, 1.0f / 60.0f };
            glm::vec4 Color{ 1.0f, 1.0f, 0.0f, 0.0f };
            glm::vec4 ColorGain{ 1.0f };
            glm::vec4 ColorOffset{ 0.0f };
            glm::vec4 LocalExposure{ 0.0f, 0.0f, 2.0f, 0.0f };
        };
        static_assert(sizeof(Constants) == 128);

        struct PassData
        {
            TextureHandle Input;
            TextureHandle Secondary;
            TextureHandle Exposure;
            TextureHandle Output;
            glm::uvec2 SourceDimensions{ 1u };
            glm::uvec2 OutputDimensions{ 1u };
            uint32_t Op = 0;
        };

        struct BindingKey
        {
            std::array<nvrhi::ITexture*, 4> Textures{};
            bool operator==(const BindingKey&) const = default;
        };

        struct BindingHash
        {
            size_t operator()(const BindingKey& key) const noexcept
            {
                size_t result = 0;
                for (const auto* texture : key.Textures)
                    result ^= reinterpret_cast<size_t>(texture) + 0x9e3779b9u +
                        (result << 6u) + (result >> 2u);
                return result;
            }
        };

        bool NeedsLocalExposure(const ScenePostProcessSettings& settings)
        {
            return settings.LocalExposureMaxEV > 0.0f &&
                (settings.LocalExposureShadowStrength > 0.0f || settings.LocalExposureHighlightStrength > 0.0f);
        }

        bool NeedsProcessing(const ScenePostProcessSettings& settings)
        {
            return settings.BloomIntensity > 0.0f || settings.bAutoExposure || NeedsLocalExposure(settings) ||
                settings.ExposureCompensationEV != 0.0f ||
                settings.Contrast != 1.0f || settings.Saturation != 1.0f ||
                settings.Temperature != 0.0f || settings.VignetteIntensity != 0.0f ||
                settings.ColorGain.x != 1.0f || settings.ColorGain.y != 1.0f ||
                settings.ColorGain.z != 1.0f || settings.ColorOffset.x != 0.0f ||
                settings.ColorOffset.y != 0.0f || settings.ColorOffset.z != 0.0f;
        }

        float SafeClamp(float value, float fallback, float minimum, float maximum)
        {
            return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
        }
    }

    struct ScenePostProcessPass::Impl
    {
        using BindingMap = std::unordered_map<BindingKey, nvrhi::BindingSetHandle, BindingHash>;
        struct RetiredState
        {
            uint64_t Submission = 0;
            BindingMap Bindings;
            nvrhi::TextureHandle Exposure;
        };

        explicit Impl(const ScenePostProcessPassDesc& desc)
            : Device(desc.Device)
        {
            if (!Device || desc.ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("ScenePostProcessPass requires a device and shader cache root");
            if (!desc.SharedShaderManager)
                OwnedShaderManager = CreateScope<ShaderManager>(Device);
            ShaderManager& shaderManager = desc.SharedShaderManager
                ? *desc.SharedShaderManager : *OwnedShaderManager;
            const auto binaryFormat = GetShaderBinaryFormat(desc.Backend);
            const auto artifactPath = desc.ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(binaryFormat)) / "ScenePostProcess.cs";
            std::ifstream keyStream(MakeShaderCacheKeyPath(artifactPath));
            std::string hash;
            for (std::string line; std::getline(keyStream, line);)
            {
                if (line.starts_with("key="))
                {
                    hash = line.substr(4);
                    break;
                }
            }
            if (hash.empty())
                throw std::runtime_error("ScenePostProcess shader cache-key sidecar is missing or invalid");
            Shader = shaderManager.LoadShaderFile(ShaderCacheKey{
                .Source = "Modern/ScenePostProcess.hlsl", .Variant = "BASE",
                .Stage = nvrhi::ShaderType::Compute, .EntryPoint = "CS",
                .TargetProfile = "cs_6_0", .ContentHash = hash,
                .Backend = desc.Backend, .BinaryFormat = binaryFormat }, artifactPath, "Scene Post Process CS");
            nvrhi::BindingLayoutDesc layout;
            layout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            Layout = Device->createBindingLayout(layout);
            nvrhi::SamplerDesc sampler;
            sampler.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setAllFilters(true);
            Sampler = Device->createSampler(sampler);
            nvrhi::ComputePipelineDesc pipeline;
            pipeline.setComputeShader(Shader).addBindingLayout(Layout);
            Pipeline = Device->createComputePipeline(pipeline);
            if (!Shader || !Layout || !Sampler || !Pipeline)
                throw std::runtime_error("NVRHI failed to create post-process immutable state");
        }

        nvrhi::IBindingSet* GetBindingSet(const BindingKey& key)
        {
            const auto found = Bindings.find(key);
            if (found != Bindings.end())
                return found->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, key.Textures[0]))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(1, key.Textures[1]))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(2, key.Textures[2]))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(0, key.Textures[3]))
                .addItem(nvrhi::BindingSetItem::Sampler(0, Sampler));
            auto binding = Device->createBindingSet(desc, Layout);
            if (!binding)
                throw std::runtime_error("NVRHI failed to bind post-process resources");
            return Bindings.emplace(key, std::move(binding)).first->second;
        }

        void Retire(bool bHistory)
        {
            if (Bindings.empty() && (!bHistory || !ExposureHistory))
                return;
            RetiredState retired;
            retired.Submission = LastSubmission;
            retired.Bindings = std::move(Bindings);
            Bindings.clear();
            if (bHistory)
                retired.Exposure = std::move(ExposureHistory);
            Retired.push_back(std::move(retired));
        }

        void EnsureHistory()
        {
            if (ExposureHistory)
                return;
            nvrhi::TextureDesc desc;
            desc.setWidth(1).setHeight(1).setFormat(nvrhi::Format::RGBA32_FLOAT)
                .setIsUAV(true).setInitialState(nvrhi::ResourceStates::Common)
                .setDebugName("Auto Exposure History");
            ExposureHistory = Device->createTexture(desc);
            if (!ExposureHistory)
                throw std::runtime_error("NVRHI failed to allocate auto exposure history");
            bResetHistory = true;
            bHistorySubmitted = false;
        }

        TextureHandle AddStage(RenderGraph& graph, const char* name, uint32_t op,
            TextureHandle source, TextureHandle secondary, TextureHandle exposure, TextureHandle output,
            glm::uvec2 sourceDimensions, glm::uvec2 outputDimensions)
        {
            graph.AddPass<PassData>(name, RenderGraphQueue::Compute,
                [=](RenderGraphBuilder& builder, PassData& data)
                {
                    data.Input = builder.ShaderResource(source);
                    data.Secondary = builder.ShaderResource(secondary);
                    data.Exposure = builder.ShaderResource(exposure);
                    data.Output = op == AdaptExposure ? builder.ReadWrite(output) : builder.Write(output);
                    data.SourceDimensions = sourceDimensions;
                    data.OutputDimensions = outputDimensions;
                    data.Op = op;
                },
                [this](RenderGraphContext& context, const PassData& data)
                {
                    Constants constants = FrameConstants;
                    constants.SourceDimensions = data.SourceDimensions;
                    constants.OutputDimensions = data.OutputDimensions;
                    constants.Op = data.Op;
                    constants.ResetHistory = bResetHistory ? 1u : 0u;
                    BindingKey key{{
                        context.GetTexture(data.Input, RenderGraphAccess::Read),
                        context.GetTexture(data.Secondary, RenderGraphAccess::Read),
                        context.GetTexture(data.Exposure, RenderGraphAccess::Read),
                        context.GetTexture(data.Output, data.Op == AdaptExposure
                            ? RenderGraphAccess::ReadWrite : RenderGraphAccess::Write) }};
                    nvrhi::ComputeState state;
                    state.setPipeline(Pipeline).addBindingSet(GetBindingSet(key));
                    auto* commands = context.GetCommandList();
                    commands->setComputeState(state);
                    commands->setPushConstants(&constants, sizeof(constants));
                    if (data.Op == LuminanceTiles)
                        commands->dispatch(data.OutputDimensions.x, data.OutputDimensions.y, 1);
                    else if (data.Op == AdaptExposure)
                        commands->dispatch(1, 1, 1);
                    else
                        commands->dispatch((data.OutputDimensions.x + 7u) / 8u,
                            (data.OutputDimensions.y + 7u) / 8u, 1);
                    bFrameRecorded = true;
                });
            return output;
        }

        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle Layout;
        nvrhi::SamplerHandle Sampler;
        nvrhi::ComputePipelineHandle Pipeline;
        nvrhi::TextureHandle ExposureHistory;
        BindingMap Bindings;
        std::vector<RetiredState> Retired;
        ScenePostProcessSettings Settings;
        Constants FrameConstants;
        uint64_t LastSubmission = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        bool bResetHistory = true;
        bool bHistorySubmitted = false;
        bool bGraphRebuild = true;
        bool bFrameRecorded = false;
    };

    ScenePostProcessPass::ScenePostProcessPass(const ScenePostProcessPassDesc& desc)
        : mImpl(CreateScope<Impl>(desc)) {}
    ScenePostProcessPass::~ScenePostProcessPass() = default;

    void ScenePostProcessPass::PrepareFrame(const ScenePostProcessSettings& requested, float deltaSeconds,
        float manualExposure, uint64_t completedSubmission, bool bResetHistory)
    {
        Collect(completedSubmission);
        ScenePostProcessSettings settings = requested;
        settings.ExposureCompensationEV = SafeClamp(settings.ExposureCompensationEV, 0.0f, -4.0f, 4.0f);
        settings.BloomIntensity = SafeClamp(settings.BloomIntensity, 0.05f, 0.0f, 2.0f);
        settings.BloomThreshold = SafeClamp(settings.BloomThreshold, 1.0f, 0.0f, 100.0f);
        settings.ExposureKey = SafeClamp(settings.ExposureKey, 0.18f, 0.01f, 1.0f);
        settings.MinimumExposure = SafeClamp(settings.MinimumExposure, std::exp2(-20.0f), std::exp2(-32.0f), std::exp2(24.0f));
        settings.MaximumExposure = SafeClamp(settings.MaximumExposure, 16.0f, settings.MinimumExposure, std::exp2(24.0f));
        settings.ExposureAdaptationSpeed = SafeClamp(settings.ExposureAdaptationSpeed, 2.0f, 0.01f, 20.0f);
        settings.Contrast = SafeClamp(settings.Contrast, 1.0f, 0.1f, 3.0f);
        settings.Saturation = SafeClamp(settings.Saturation, 1.0f, 0.0f, 2.0f);
        settings.Temperature = SafeClamp(settings.Temperature, 0.0f, -1.0f, 1.0f);
        settings.LocalExposureShadowStrength = SafeClamp(settings.LocalExposureShadowStrength, 0.0f, 0.0f, 1.0f);
        settings.LocalExposureHighlightStrength = SafeClamp(settings.LocalExposureHighlightStrength, 0.0f, 0.0f, 1.0f);
        settings.LocalExposureMaxEV = SafeClamp(settings.LocalExposureMaxEV, 2.0f, 0.0f, 4.0f);
        settings.VignetteIntensity = SafeClamp(settings.VignetteIntensity, 0.0f, 0.0f, 1.0f);
        for (int channel = 0; channel < 3; ++channel)
        {
            settings.ColorGain[channel] = SafeClamp(settings.ColorGain[channel], 1.0f, 0.0f, 2.0f);
            settings.ColorOffset[channel] = SafeClamp(settings.ColorOffset[channel], 0.0f, -0.25f, 0.25f);
        }
        mImpl->bGraphRebuild |= NeedsProcessing(settings) != NeedsProcessing(mImpl->Settings) ||
            NeedsLocalExposure(settings) != NeedsLocalExposure(mImpl->Settings) ||
            settings.bAutoExposure != mImpl->Settings.bAutoExposure ||
            (settings.BloomIntensity > 0.0f) != (mImpl->Settings.BloomIntensity > 0.0f);
        mImpl->bResetHistory |= bResetHistory || settings.bAutoExposure != mImpl->Settings.bAutoExposure;
        mImpl->Settings = settings;
        mImpl->FrameConstants.AutoExposure = settings.bAutoExposure ? 1u : 0u;
        mImpl->FrameConstants.BloomExposure = { settings.BloomIntensity, settings.BloomThreshold,
            SafeClamp(manualExposure, std::exp2(-14.0f), std::exp2(-32.0f), std::exp2(24.0f)), settings.ExposureKey };
        mImpl->FrameConstants.Adaptation = { settings.MinimumExposure, settings.MaximumExposure,
            settings.ExposureAdaptationSpeed, SafeClamp(deltaSeconds, 0.0f, 0.0f, 1.0f) };
        mImpl->FrameConstants.Color = { settings.Contrast, settings.Saturation,
            settings.Temperature, settings.VignetteIntensity };
        mImpl->FrameConstants.ColorGain = glm::vec4(settings.ColorGain, 0.0f);
        mImpl->FrameConstants.ColorOffset = glm::vec4(settings.ColorOffset, 0.0f);
        mImpl->FrameConstants.LocalExposure = { settings.LocalExposureShadowStrength,
            settings.LocalExposureHighlightStrength, settings.LocalExposureMaxEV, settings.ExposureCompensationEV };
    }

    TextureHandle ScenePostProcessPass::AddPasses(RenderGraph& graph, TextureHandle hdrColor,
        uint32_t width, uint32_t height)
    {
        if (!hdrColor || width == 0 || height == 0)
            throw std::invalid_argument("Post processing requires a native HDR input and nonzero extent");
        mImpl->Retire(false);
        if (mImpl->Width != width || mImpl->Height != height)
            mImpl->bResetHistory = true;
        mImpl->Width = width;
        mImpl->Height = height;
        mImpl->FrameConstants.Padding = ((width - 1u) % 4u + 1u) |
            (((height - 1u) % 4u + 1u) << 8u);
        mImpl->bGraphRebuild = false;
        if (!NeedsProcessing(mImpl->Settings))
            return hdrColor;
        const glm::uvec2 nativeDimensions{ width, height };
        const glm::uvec2 quarterDimensions{ (width + 3u) / 4u, (height + 3u) / 4u };
        auto createTexture = [&graph](const char* name, glm::uvec2 dimensions,
            nvrhi::Format format = nvrhi::Format::RGBA16_FLOAT)
        {
            return graph.CreateTexture(RenderGraphTextureDesc{ .DebugName = name,
                .Width = dimensions.x, .Height = dimensions.y, .Format = format });
        };
        TextureHandle bloom = hdrColor;
        TextureHandle exposure = hdrColor;
        if (mImpl->Settings.BloomIntensity > 0.0f || mImpl->Settings.bAutoExposure || NeedsLocalExposure(mImpl->Settings))
        {
            const auto downsample = createTexture("Post Process Quarter HDR", quarterDimensions);
            mImpl->AddStage(graph, "PostProcessDownsample", Downsample, hdrColor, hdrColor, hdrColor,
                downsample, nativeDimensions, quarterDimensions);
            if (mImpl->Settings.BloomIntensity > 0.0f || NeedsLocalExposure(mImpl->Settings))
            {
                const auto horizontal = createTexture("Bloom Horizontal", quarterDimensions);
                bloom = createTexture("Bloom Vertical", quarterDimensions);
                mImpl->AddStage(graph, "BloomHorizontal", BlurHorizontal, downsample, downsample, downsample,
                    horizontal, quarterDimensions, quarterDimensions);
                mImpl->AddStage(graph, "BloomVertical", BlurVertical, horizontal, horizontal, horizontal,
                    bloom, quarterDimensions, quarterDimensions);
            }
            if (mImpl->Settings.bAutoExposure)
            {
                const glm::uvec2 tileDimensions{ (quarterDimensions.x + 7u) / 8u,
                    (quarterDimensions.y + 7u) / 8u };
                const auto tiles = createTexture("Exposure Log Luminance Tiles", tileDimensions,
                    nvrhi::Format::RGBA32_FLOAT);
                mImpl->AddStage(graph, "ExposureTileReduce", LuminanceTiles, downsample, downsample, downsample,
                    tiles, quarterDimensions, tileDimensions);
                mImpl->EnsureHistory();
                exposure = graph.ImportTexture(RenderGraphTextureDesc{
                    .DebugName = "Auto Exposure History", .Width = 1, .Height = 1,
                    .Format = nvrhi::Format::RGBA32_FLOAT }, mImpl->ExposureHistory,
                    mImpl->bHistorySubmitted ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common,
                    nvrhi::ResourceStates::ShaderResource);
                mImpl->AddStage(graph, "AutoExposureAdapt", AdaptExposure, tiles, tiles, tiles,
                    exposure, tileDimensions, {1u, 1u});
            }
        }
        const auto output = createTexture("Post Process Native HDR", nativeDimensions);
        return mImpl->AddStage(graph, "PostProcessComposite", Composite, hdrColor, bloom, exposure,
            output, nativeDimensions, nativeDimensions);
    }

    bool ScenePostProcessPass::ConsumeGraphRebuildRequest() noexcept
    {
        const bool result = mImpl->bGraphRebuild;
        mImpl->bGraphRebuild = false;
        return result;
    }
    bool ScenePostProcessPass::RequiresContinuousUpdate() const noexcept { return mImpl->Settings.bAutoExposure; }
    void ScenePostProcessPass::ResetHistory() noexcept { mImpl->bResetHistory = true; }
    void ScenePostProcessPass::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        if (mImpl->bFrameRecorded)
        {
            if (mImpl->Settings.bAutoExposure && !mImpl->bHistorySubmitted)
            {
                mImpl->bHistorySubmitted = true;
                // Only actual submission establishes the imported SRV layout.
                // Rebuild once to stop discarding this history on later frames.
                mImpl->bGraphRebuild = true;
            }
            mImpl->LastSubmission = submission;
            mImpl->bResetHistory = false;
            mImpl->bFrameRecorded = false;
        }
    }
    void ScenePostProcessPass::AbortFrame() noexcept { mImpl->bFrameRecorded = false; }
    void ScenePostProcessPass::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->Retired, [completedSubmission](const Impl::RetiredState& state)
            { return state.Submission <= completedSubmission; });
    }
    void ScenePostProcessPass::Clear(uint64_t completedSubmission)
    {
        mImpl->Retire(true);
        mImpl->bResetHistory = true;
        mImpl->bGraphRebuild = true;
        Collect(completedSubmission);
    }
    std::function<void()> ScenePostProcessPass::CaptureGraphState()
    {
        return [impl = mImpl.get(), bindings = mImpl->Bindings, retired = mImpl->Retired]() mutable noexcept
        { impl->Bindings.swap(bindings); impl->Retired.swap(retired); };
    }
    void ScenePostProcessPass::ClearGraphResourceCaches() noexcept
    {
        mImpl->Bindings.clear();
        for (auto& retired : mImpl->Retired) retired.Bindings.clear();
    }
}
