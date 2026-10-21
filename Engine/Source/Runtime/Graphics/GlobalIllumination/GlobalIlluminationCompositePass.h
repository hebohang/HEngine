#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>

namespace HEngine
{
    class ShaderManager;

    enum class GlobalIlluminationCompositeMode : uint8_t
    {
        Additive,
        Replace,
    };

    struct GlobalIlluminationCompositePassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    class GlobalIlluminationCompositePass final
    {
    public:
        explicit GlobalIlluminationCompositePass(
            const GlobalIlluminationCompositePassDesc& desc);
        ~GlobalIlluminationCompositePass();

        GlobalIlluminationCompositePass(
            const GlobalIlluminationCompositePass&) = delete;
        GlobalIlluminationCompositePass& operator=(
            const GlobalIlluminationCompositePass&) = delete;

        TextureHandle AddPass(
            RenderGraph& renderGraph,
            TextureHandle sceneColor,
            TextureHandle globalIllumination,
            GlobalIlluminationCompositeMode mode,
            uint32_t width,
            uint32_t height,
            uint32_t inputChannel = 0);
        TextureHandle AddMultisamplePass(
            RenderGraph& renderGraph,
            TextureHandle multisampleSceneColor,
            TextureHandle multisampleSceneDepth,
            TextureHandle representativeSceneDepth,
            TextureHandle globalIllumination,
            uint32_t width,
            uint32_t height,
            uint32_t sampleCount,
            uint32_t inputChannel = 0);
        void ClearCaches();
        [[nodiscard]] std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
