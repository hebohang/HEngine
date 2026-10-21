#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/DepthPyramidPass.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <string>
#include <unordered_map>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr uint32_t c_ThreadGroupSize = 8;

        struct alignas(16) DepthPyramidConstants
        {
            std::array<uint32_t, 2> SourceDimensions{};
            std::array<uint32_t, 2> OutputDimensions{};
            uint32_t CopySource = 0;
            uint32_t MaximumReduction = 0;
            uint32_t ReductionFactor = 2;
            uint32_t Padding = 0;
        };

        static_assert(sizeof(DepthPyramidConstants) == 32);

        struct DepthPyramidPassData
        {
            TextureHandle Source;
            TextureHandle Output;
            uint32_t SourceMip = 0;
            uint32_t OutputMip = 0;
            uint32_t SourceWidth = 0;
            uint32_t SourceHeight = 0;
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            bool bCopySource = false;
            bool bMaximumReduction = false;
            uint32_t ReductionFactor = 2;
        };

        struct BindingKey
        {
            nvrhi::ITexture* Source = nullptr;
            nvrhi::ITexture* Output = nullptr;
            uint32_t SourceMip = 0;
            uint32_t OutputMip = 0;

            [[nodiscard]] bool operator==(const BindingKey&) const = default;
        };

        struct BindingKeyHash
        {
            [[nodiscard]] size_t operator()(const BindingKey& key) const noexcept
            {
                size_t hash = reinterpret_cast<size_t>(key.Source);
                const auto combine = [&hash](size_t value)
                {
                    hash ^= value + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                };
                combine(reinterpret_cast<size_t>(key.Output));
                combine(key.SourceMip);
                combine(key.OutputMip);
                return hash;
            }
        };

        [[nodiscard]] std::string ReadShaderContentHash(
            const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
            {
                throw std::runtime_error(
                    "Failed to open depth-pyramid shader cache-key sidecar: " +
                    Utils::PathToUtf8(path));
            }
            std::string line;
            while (std::getline(stream, line))
            {
                constexpr std::string_view Prefix = "key=";
                if (line.starts_with(Prefix) && line.size() > Prefix.size())
                    return line.substr(Prefix.size());
            }
            throw std::runtime_error(
                "Depth-pyramid shader cache-key sidecar has no key: " +
                Utils::PathToUtf8(path));
        }

        [[nodiscard]] uint32_t DispatchCount(uint32_t dimension) noexcept
        {
            return (dimension + c_ThreadGroupSize - 1u) / c_ThreadGroupSize;
        }
    }

    uint32_t CalculateMipCount(uint32_t width, uint32_t height) noexcept
    {
        if (width == 0 || height == 0)
            return 0;

        uint32_t mipCount = 1;
        while (width > 1 || height > 1)
        {
            width = std::max(width / 2u, 1u);
            height = std::max(height / 2u, 1u);
            ++mipCount;
        }
        return mipCount;
    }

    float ReduceMinDepth(std::span<const float> depths) noexcept
    {
        float minimumDepth = c_DepthPyramidBackgroundDepth;
        for (float depth : depths)
            minimumDepth = std::min(minimumDepth, depth);
        return minimumDepth;
    }

    std::vector<float> ReduceMinDepth(
        std::span<const float> sourceDepth,
        uint32_t sourceWidth,
        uint32_t sourceHeight)
    {
        if (sourceWidth == 0 || sourceHeight == 0)
            throw std::invalid_argument("Depth reduction dimensions must be non-zero");

        const uint64_t expectedCount =
            static_cast<uint64_t>(sourceWidth) * sourceHeight;
        if (expectedCount > std::numeric_limits<size_t>::max() ||
            sourceDepth.size() != static_cast<size_t>(expectedCount))
        {
            throw std::invalid_argument(
                "Depth reduction source size does not match its dimensions");
        }

        const uint32_t outputWidth = std::max(sourceWidth / 2u, 1u);
        const uint32_t outputHeight = std::max(sourceHeight / 2u, 1u);
        std::vector<float> output(
            static_cast<size_t>(outputWidth) * outputHeight,
            c_DepthPyramidBackgroundDepth);

        for (uint32_t outputY = 0; outputY < outputHeight; ++outputY)
        {
            const uint32_t sourceBeginY = outputY * 2u;
            const uint32_t sourceEndY = outputY + 1u == outputHeight
                ? sourceHeight
                : std::min(sourceBeginY + 2u, sourceHeight);
            for (uint32_t outputX = 0; outputX < outputWidth; ++outputX)
            {
                const uint32_t sourceBeginX = outputX * 2u;
                const uint32_t sourceEndX = outputX + 1u == outputWidth
                    ? sourceWidth
                    : std::min(sourceBeginX + 2u, sourceWidth);
                float minimumDepth = c_DepthPyramidBackgroundDepth;
                for (uint32_t sourceY = sourceBeginY;
                    sourceY < sourceEndY;
                    ++sourceY)
                {
                    for (uint32_t sourceX = sourceBeginX;
                        sourceX < sourceEndX;
                        ++sourceX)
                    {
                        minimumDepth = std::min(
                            minimumDepth,
                            sourceDepth[static_cast<size_t>(sourceY) *
                                sourceWidth + sourceX]);
                    }
                }
                output[static_cast<size_t>(outputY) * outputWidth + outputX] =
                    minimumDepth;
            }
        }
        return output;
    }

    struct DepthPyramidPass::Impl
    {
        explicit Impl(const DepthPyramidPassDesc& desc)
            : Device(desc.Device)
            , OwnedShaderManager(desc.SharedShaderManager
                ? nullptr
                : CreateScope<ShaderManager>(desc.Device))
            , ShaderManagerInstance(desc.SharedShaderManager
                ? *desc.SharedShaderManager
                : *OwnedShaderManager)
            , ShaderArtifactCacheRoot(desc.ShaderArtifactCacheRoot)
            , Backend(desc.Backend)
            , BinaryFormat(GetShaderBinaryFormat(desc.Backend))
            , bMaximumReduction(desc.bMaximumReduction)
            , BaseDownsample(desc.BaseDownsample)
        {
            if (!Device)
                throw std::invalid_argument("DepthPyramidPass requires a valid NVRHI device");
            if (BaseDownsample != 1u && BaseDownsample != 2u && BaseDownsample != 4u)
                throw std::invalid_argument("Depth pyramid base downsample must be one, two or four");
            if (ShaderArtifactCacheRoot.empty())
            {
                throw std::invalid_argument(
                    "DepthPyramidPass requires a shader artifact cache root");
            }
            CreateImmutableState();
        }

        void CreateImmutableState()
        {
            const std::filesystem::path artifactPath =
                ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(BinaryFormat)) /
                "DepthPyramid.cs";
            Shader = ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = "GlobalIllumination/DepthPyramid.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Compute,
                    .EntryPoint = "CS",
                    .TargetProfile = "cs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(artifactPath)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                artifactPath,
                "GI Depth Pyramid CS");

            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(DepthPyramidConstants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
            BindingLayout = Device->createBindingLayout(layoutDesc);

            nvrhi::ComputePipelineDesc pipelineDesc;
            pipelineDesc.setComputeShader(Shader).addBindingLayout(BindingLayout);
            Pipeline = Device->createComputePipeline(pipelineDesc);
            if (!Shader || !BindingLayout || !Pipeline)
            {
                throw std::runtime_error(
                    "NVRHI failed to create GI depth-pyramid immutable state");
            }
        }

        [[nodiscard]] nvrhi::IBindingSet* GetBindingSet(const BindingKey& key)
        {
            const auto existing = BindingSets.find(key);
            if (existing != BindingSets.end())
                return existing->second;

            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.addItem(nvrhi::BindingSetItem::PushConstants(
                    0,
                    sizeof(DepthPyramidConstants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    0,
                    key.Source,
                    nvrhi::Format::UNKNOWN,
                    nvrhi::TextureSubresourceSet(key.SourceMip, 1, 0, 1),
                    nvrhi::TextureDimension::Texture2D))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    0,
                    key.Output,
                    nvrhi::Format::UNKNOWN,
                    nvrhi::TextureSubresourceSet(key.OutputMip, 1, 0, 1),
                    nvrhi::TextureDimension::Texture2D));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                bindingDesc,
                BindingLayout);
            if (!bindingSet)
            {
                throw std::runtime_error(
                    "NVRHI failed to create a GI depth-pyramid binding set");
            }
            return BindingSets.emplace(key, std::move(bindingSet)).first->second;
        }

        void Dispatch(
            nvrhi::ICommandList* commandList,
            const BindingKey& key,
            const DepthPyramidConstants& constants)
        {
            nvrhi::ComputeState state;
            state.setPipeline(Pipeline).addBindingSet(GetBindingSet(key));
            commandList->setComputeState(state);
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->dispatch(
                DispatchCount(constants.OutputDimensions[0]),
                DispatchCount(constants.OutputDimensions[1]),
                1);
        }

        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        ShaderManager& ShaderManagerInstance;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        bool bMaximumReduction = false;
        uint32_t BaseDownsample = 1;
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle BindingLayout;
        nvrhi::ComputePipelineHandle Pipeline;
        std::unordered_map<BindingKey, nvrhi::BindingSetHandle, BindingKeyHash>
            BindingSets;
    };

    DepthPyramidPass::DepthPyramidPass(const DepthPyramidPassDesc& desc)
        : mImpl(CreateScope<Impl>(desc))
    {
    }

    DepthPyramidPass::~DepthPyramidPass() = default;

    DepthPyramidOutputs DepthPyramidPass::AddPasses(
        RenderGraph& renderGraph,
        TextureHandle sceneDepth,
        uint32_t width,
        uint32_t height)
    {
        if (!sceneDepth || width == 0 || height == 0)
            throw std::invalid_argument("DepthPyramidPass inputs are incomplete");

        const RenderGraphTextureDesc& sceneDepthDesc =
            renderGraph.GetTextureDesc(sceneDepth);
        if (sceneDepthDesc.Width != width || sceneDepthDesc.Height != height)
        {
            throw std::invalid_argument(
                "DepthPyramidPass dimensions do not match the scene depth texture");
        }
        if (sceneDepthDesc.Dimension != nvrhi::TextureDimension::Texture2D ||
            sceneDepthDesc.SampleCount != 1)
        {
            throw std::invalid_argument(
                "DepthPyramidPass requires a single-sample 2D scene depth texture");
        }

        const uint32_t baseWidth = (width + mImpl->BaseDownsample - 1u) / mImpl->BaseDownsample;
        const uint32_t baseHeight = (height + mImpl->BaseDownsample - 1u) / mImpl->BaseDownsample;
        const uint32_t mipCount = CalculateMipCount(baseWidth, baseHeight);
        const TextureHandle pyramid = renderGraph.CreateTexture(
            RenderGraphTextureDesc{
                .DebugName = mImpl->bMaximumReduction ? "Occlusion MAX Depth Pyramid" : "GI Normal-Z Depth Pyramid",
                .Width = baseWidth,
                .Height = baseHeight,
                .MipLevels = mipCount,
                .Format = nvrhi::Format::R32_FLOAT,
                .CapabilityUsage = RenderGraphUsage::ShaderResource | RenderGraphUsage::UnorderedAccess,
            });

        bool mergeGiDepthPyramid = false;
#if defined(HE_DEV_AGENT_RUNTIME)
        static const bool mergeRequested = [] {
            const char* value = std::getenv("HE_RENDER_MERGE_GI_DEPTH_PYRAMID");
            return value && std::string_view(value) == "1";
        }();
        mergeGiDepthPyramid = mergeRequested && !mImpl->bMaximumReduction &&
            mImpl->BaseDownsample == 1u;
#endif
        if (mergeGiDepthPyramid)
        {
            struct MergedPassData
            {
                TextureHandle Source;
                TextureHandle Output;
            };
            renderGraph.AddPass<MergedPassData>(
                "DepthPyramidMerged", RenderGraphQueue::Compute,
                [sceneDepth, pyramid, mipCount](RenderGraphBuilder& builder, MergedPassData& data)
                {
                    data.Source = builder.ShaderResource(sceneDepth,
                        { .BaseMipLevel = 0, .MipLevelCount = 1,
                          .BaseArraySlice = 0, .ArraySliceCount = 1 });
                    // Internal reads consume mips produced in this callback, not old contents.
                    data.Output = builder.Write(pyramid, RenderGraphUsage::UnorderedAccess,
                        { .BaseMipLevel = 0, .MipLevelCount = mipCount,
                          .BaseArraySlice = 0, .ArraySliceCount = 1 });
                },
                [impl = mImpl.get(), width, height, mipCount](
                    RenderGraphContext& context, const MergedPassData& data)
                {
                    auto* commandList = context.GetCommandList();
                    auto* sceneDepth = context.GetTexture(data.Source, RenderGraphAccess::Read);
                    auto* pyramid = context.GetTexture(data.Output, RenderGraphAccess::Write);
                    uint32_t sourceWidth = width;
                    uint32_t sourceHeight = height;
                    for (uint32_t mip = 0; mip < mipCount; ++mip)
                    {
                        const uint32_t outputWidth = mip == 0 ? sourceWidth : std::max(sourceWidth / 2u, 1u);
                        const uint32_t outputHeight = mip == 0 ? sourceHeight : std::max(sourceHeight / 2u, 1u);
                        if (mip > 0)
                            commandList->setTextureState(pyramid,
                                nvrhi::TextureSubresourceSet(mip - 1u, 1, 0, 1),
                                nvrhi::ResourceStates::ShaderResource);
                        commandList->setTextureState(pyramid,
                            nvrhi::TextureSubresourceSet(mip, 1, 0, 1),
                            nvrhi::ResourceStates::UnorderedAccess);
                        commandList->commitBarriers();
                        impl->Dispatch(commandList,
                            BindingKey{ .Source = mip == 0 ? sceneDepth : pyramid,
                                .Output = pyramid, .SourceMip = mip == 0 ? 0 : mip - 1u,
                                .OutputMip = mip },
                            DepthPyramidConstants{
                                .SourceDimensions = { sourceWidth, sourceHeight },
                                .OutputDimensions = { outputWidth, outputHeight },
                                .CopySource = mip == 0 ? 1u : 0u,
                                .MaximumReduction = 0u,
                                .ReductionFactor = mip == 0 ? 1u : 2u });
                        sourceWidth = outputWidth;
                        sourceHeight = outputHeight;
                    }
                    // Match the graph declaration on exit; consumers establish their SRV state.
                    commandList->setTextureState(pyramid,
                        nvrhi::TextureSubresourceSet(0, mipCount, 0, 1),
                        nvrhi::ResourceStates::UnorderedAccess);
                    commandList->commitBarriers();
                });
            return { .Depth = pyramid, .Width = baseWidth, .Height = baseHeight,
                .MipCount = mipCount };
        }

        uint32_t sourceWidth = width;
        uint32_t sourceHeight = height;
        for (uint32_t outputMip = 0; outputMip < mipCount; ++outputMip)
        {
            const bool bFirstMip = outputMip == 0;
            const bool bCopySource = bFirstMip && mImpl->BaseDownsample == 1u;
            const uint32_t outputWidth = bFirstMip
                ? baseWidth
                : std::max(sourceWidth / 2u, 1u);
            const uint32_t outputHeight = bFirstMip
                ? baseHeight
                : std::max(sourceHeight / 2u, 1u);
            const uint32_t sourceMip = bFirstMip ? 0 : outputMip - 1u;
            const TextureHandle source = bFirstMip ? sceneDepth : pyramid;
            const uint32_t reductionFactor = bFirstMip ? mImpl->BaseDownsample : 2u;
            const bool bMaximumReduction = mImpl->bMaximumReduction;
            const std::string passName = (bMaximumReduction ? "Occlusion" : "") + std::string(bFirstMip
                ? "DepthPyramidMip0"
                : "DepthPyramidReduceMip" + std::to_string(outputMip));

            renderGraph.AddPass<DepthPyramidPassData>(
                passName,
                RenderGraphQueue::Compute,
                [source, pyramid, sourceMip, outputMip,
                    sourceWidth, sourceHeight, outputWidth, outputHeight,
                    bCopySource, bMaximumReduction, reductionFactor](
                    RenderGraphBuilder& builder,
                    DepthPyramidPassData& data)
                {
                    const RenderGraphSubresourceRange sourceRange{
                        .BaseMipLevel = sourceMip,
                        .MipLevelCount = 1,
                        .BaseArraySlice = 0,
                        .ArraySliceCount = 1,
                    };
                    const RenderGraphSubresourceRange outputRange{
                        .BaseMipLevel = outputMip,
                        .MipLevelCount = 1,
                        .BaseArraySlice = 0,
                        .ArraySliceCount = 1,
                    };
                    data.Source = builder.ShaderResource(source, sourceRange);
                    data.Output = builder.Write(
                        pyramid,
                        RenderGraphUsage::UnorderedAccess,
                        outputRange);
                    data.SourceMip = sourceMip;
                    data.OutputMip = outputMip;
                    data.SourceWidth = sourceWidth;
                    data.SourceHeight = sourceHeight;
                    data.OutputWidth = outputWidth;
                    data.OutputHeight = outputHeight;
                    data.bCopySource = bCopySource;
                    data.bMaximumReduction = bMaximumReduction;
                    data.ReductionFactor = reductionFactor;
                },
                [impl = mImpl.get()](
                    RenderGraphContext& context,
                    const DepthPyramidPassData& data)
                {
                    nvrhi::ITexture* source = context.GetTexture(
                        data.Source,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* output = context.GetTexture(
                        data.Output,
                        RenderGraphAccess::Write);
                    impl->Dispatch(
                        context.GetCommandList(),
                        BindingKey{
                            .Source = source,
                            .Output = output,
                            .SourceMip = data.SourceMip,
                            .OutputMip = data.OutputMip,
                        },
                        DepthPyramidConstants{
                            .SourceDimensions = {
                                data.SourceWidth,
                                data.SourceHeight,
                            },
                            .OutputDimensions = {
                                data.OutputWidth,
                                data.OutputHeight,
                            },
                            .CopySource = data.bCopySource ? 1u : 0u,
                            .MaximumReduction = data.bMaximumReduction ? 1u : 0u,
                            .ReductionFactor = data.ReductionFactor,
                        });
                });

            sourceWidth = outputWidth;
            sourceHeight = outputHeight;
        }

        return {
            .Depth = pyramid,
            .Width = baseWidth,
            .Height = baseHeight,
            .MipCount = mipCount,
        };
    }

    void DepthPyramidPass::ClearCaches()
    {
        mImpl->BindingSets.clear();
    }
    std::function<void()> DepthPyramidPass::CaptureGraphState()
    {
        return [impl = mImpl.get(), bindings = mImpl->BindingSets]() mutable noexcept
        { impl->BindingSets.swap(bindings); };
    }
}
