#pragma once

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"

#include <string>

namespace HEngine
{
    // Resolves a multisampled color texture into a graph-owned single-sample
    // texture. Single-sample inputs are returned unchanged. Depth resolves are
    // intentionally rejected because NVRHI's cross-backend resolve contract is
    // color-only.
    [[nodiscard]] TextureHandle AddTextureResolvePass(
        RenderGraph& renderGraph,
        TextureHandle source,
        std::string debugName);
}
