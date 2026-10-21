#pragma once

#include "Runtime/Graphics/Renderer/RenderPipeline.h"

namespace HEngine
{
    using RenderPipelineFactory = std::function<Scope<RenderPipeline>(const SceneRendererDesc&)>;
    void RegisterRenderPipeline(std::string type, RenderPipelineFactory factory);
    [[nodiscard]] std::vector<std::string> EnumerateRenderPipelineTypes();
    [[nodiscard]] bool IsRenderPipelineRegistered(std::string_view type);
    [[nodiscard]] Scope<RenderPipeline> CreateRenderPipeline(const SceneRendererDesc& desc);
}
