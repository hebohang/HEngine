#include "hepch.h"

#include "Runtime/Renderer/RenderPass.h"
#include "Runtime/Renderer/Renderer.h"
#include "Runtime/Platform/OpenGL/OpenGLPostProcessing.h"

namespace HEngine
{
    void RenderPass::AddPostProcessing(PostProcessingType type)
    {
        switch (RendererAPI::Current())
        {
        case RendererAPI::RendererAPIType::None:    HE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return;
        case RendererAPI::RendererAPIType::OpenGL:  mPostProcessings.emplace_back(CreateScope<OpenGLPostProcessing>(type)); return;
        case RendererAPI::RendererAPIType::Vulkan:  return;
        case RendererAPI::RendererAPIType::DX11:    return;
        case RendererAPI::RendererAPIType::DX12:    return;
        }

        HE_CORE_ASSERT(false, "Unknown RendererAPI!");
        return;
    }

    uint32_t RenderPass::ExcuteAndReturnFinalTex()
    {
        uint32_t width = mSpecification.TargetFramebuffer->GetSpecification().Width;
        uint32_t height = mSpecification.TargetFramebuffer->GetSpecification().Height;
        PostProcessing::mFramebuffer->Bind();
        PostProcessing::mFramebuffer->Resize(width, height);

        uint32_t final = 0;
        for (auto& effect : mPostProcessings)
        {
            final = effect->ExcuteAndReturnFinalTex(mSpecification.TargetFramebuffer);
        }
        return final;
    }
}