#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace HEngine
{
    class ShaderManager;

    inline constexpr float c_DepthPyramidBackgroundDepth = 1.0f;

    // The renderer currently writes normal-Z device depth in [0, 1]. The
    // background is 1 and the nearest conservative occluder is therefore the
    // minimum value in every depth-pyramid footprint.
    [[nodiscard]] uint32_t CalculateMipCount(
        uint32_t width,
        uint32_t height) noexcept;

    [[nodiscard]] float ReduceMinDepth(
        std::span<const float> depths) noexcept;

    // Produces the next standard texture mip (floor-divided dimensions). When
    // a source dimension is odd, the last destination texel includes the
    // remaining third source row or column so no occluder is discarded.
    [[nodiscard]] std::vector<float> ReduceMinDepth(
        std::span<const float> sourceDepth,
        uint32_t sourceWidth,
        uint32_t sourceHeight);

    struct DepthPyramidPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    struct DepthPyramidOutputs
    {
        TextureHandle Depth;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipCount = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(Depth);
        }
    };

    // Platform-neutral NVRHI compute pass. Construction creates immutable GPU
    // state only; graph resources and passes are added exclusively by
    // AddPasses, so techniques that do not request Hi-Z have no graph cost.
    class DepthPyramidPass final
    {
    public:
        explicit DepthPyramidPass(const DepthPyramidPassDesc& desc);
        ~DepthPyramidPass();

        DepthPyramidPass(const DepthPyramidPass&) = delete;
        DepthPyramidPass& operator=(const DepthPyramidPass&) = delete;

        [[nodiscard]] DepthPyramidOutputs AddPasses(
            RenderGraph& renderGraph,
            TextureHandle sceneDepth,
            uint32_t width,
            uint32_t height);

        void ClearCaches();

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
