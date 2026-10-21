#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <span>

namespace HEngine
{
    class ShaderManager;

    struct SceneLightGridDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    struct SceneLightGridGraphResources
    {
        BufferHandle Constants;
        BufferHandle Lights;
        BufferHandle ClusterMasks;
    };

    struct SceneLightGridStats
    {
        uint32_t LightCount = 0;
        uint32_t ClusterCount = 0;
        uint32_t WordsPerCluster = 1;
        uint64_t ResidentBytes = 0;
        uint64_t RetiredBytes = 0;
        uint64_t UploadedBytes = 0;
        bool bGridDispatched = false;
    };

    // Owns persistent light data and a depth-independent 3D cluster mask.
    // Resources are consumed by the existing frame binding set: b1/t0/t1 in
    // space0. Replaced buffers remain alive until their last GPU submission.
    class SceneLightGrid final
    {
    public:
        explicit SceneLightGrid(const SceneLightGridDesc& desc);
        ~SceneLightGrid();
        SceneLightGrid(const SceneLightGrid&) = delete;
        SceneLightGrid& operator=(const SceneLightGrid&) = delete;

        [[nodiscard]] bool PrepareFrame(const RenderWorld& world,
            uint32_t width, uint32_t height, uint64_t completedSubmission);
        // UINT_MAX means the light remains illuminated without a shadow.
        // Set before PrepareFrame; point entries address the first of six faces.
        void SetLocalShadowIndices(std::span<const uint32_t> pointViews,
            std::span<const uint32_t> spotViews);
        [[nodiscard]] SceneLightGridGraphResources AddPasses(RenderGraph& graph);
        [[nodiscard]] nvrhi::IBuffer* GetConstantsBuffer() const noexcept;
        [[nodiscard]] nvrhi::IBuffer* GetLightBuffer() const noexcept;
        [[nodiscard]] nvrhi::IBuffer* GetClusterMaskBuffer() const noexcept;
        [[nodiscard]] const SceneLightGridStats& GetStats() const noexcept;
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void Collect(uint64_t completedSubmission);

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
