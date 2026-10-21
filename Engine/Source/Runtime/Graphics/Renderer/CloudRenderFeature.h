#pragma once

#include "Runtime/Graphics/Renderer/RenderFeature.h"

namespace HEngine
{
    [[nodiscard]] RenderFeatureDescriptor DescribeCloudRenderFeature();
    [[nodiscard]] std::unique_ptr<RenderFeature> CreateCloudRenderFeature(
        const RenderFeatureCreateContext& context, const Json& parameters);
}
