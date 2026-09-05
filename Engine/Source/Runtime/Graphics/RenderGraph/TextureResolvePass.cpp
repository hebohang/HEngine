#include "Hepch.h"

#include "Runtime/Graphics/RenderGraph/TextureResolvePass.h"

#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        struct TextureResolvePassData
        {
            TextureHandle Source;
            TextureHandle Destination;
        };
    }

    TextureHandle AddTextureResolvePass(
        RenderGraph& renderGraph,
        TextureHandle source,
        std::string debugName)
    {
        if (!source)
            throw std::invalid_argument("Texture resolve requires a valid source");

        const RenderGraphTextureDesc sourceDesc =
            renderGraph.GetTextureDesc(source);
        if (sourceDesc.SampleCount == 1)
            return source;
        if (sourceDesc.SampleCount == 0)
            throw std::invalid_argument("Texture resolve source has zero samples");
        if (nvrhi::getFormatInfo(sourceDesc.Format).hasDepth)
        {
            throw std::invalid_argument(
                "Texture resolve supports color formats only; retain a single-sample depth guide");
        }

        const std::string passName = debugName.empty()
            ? "TextureResolve"
            : debugName + " Resolve";
        RenderGraphTextureDesc destinationDesc = sourceDesc;
        destinationDesc.DebugName = std::move(debugName);
        destinationDesc.SampleCount = 1;
        destinationDesc.Dimension = sourceDesc.Dimension ==
                nvrhi::TextureDimension::Texture2DMSArray
            ? nvrhi::TextureDimension::Texture2DArray
            : sourceDesc.Dimension == nvrhi::TextureDimension::Texture2DMS
                ? nvrhi::TextureDimension::Texture2D
                : sourceDesc.Dimension;
        destinationDesc.CapabilityUsage = RenderGraphUsage::None;
        destinationDesc.bUseClearValue = false;
        destinationDesc.ClearValue = {};

        const TextureHandle destination =
            renderGraph.CreateTexture(destinationDesc);
        renderGraph.AddPass<TextureResolvePassData>(
            passName,
            RenderGraphQueue::Graphics,
            [source, destination](
                RenderGraphBuilder& builder,
                TextureResolvePassData& data)
            {
                data.Source = builder.ResolveSource(source);
                data.Destination = builder.ResolveDest(destination);
            },
            [](RenderGraphContext& context, const TextureResolvePassData& data)
            {
                nvrhi::ICommandList* commandList = context.GetCommandList();
                commandList->resolveTexture(
                    context.GetTexture(data.Destination, RenderGraphAccess::Write),
                    nvrhi::AllSubresources,
                    context.GetTexture(data.Source, RenderGraphAccess::Read),
                    nvrhi::AllSubresources);
            });

        return destination;
    }
}
