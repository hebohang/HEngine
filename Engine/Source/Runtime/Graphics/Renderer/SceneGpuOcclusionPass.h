#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <functional>

namespace HEngine
{
    class ShaderManager;
    struct RenderCamera;
    struct SceneGpuOcclusionPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    // Same-frame normal-Z occlusion, using complete opaque/masked depth from
    // the existing frustum-visible prepass. The caller controls its public A/B
    // toggle (off until measured). It rewrites the authoritative visible/args
    // buffers in place, never creating a parallel geometry or instance table.
    class SceneGpuOcclusionPass final
    {
    public:
        explicit SceneGpuOcclusionPass(const SceneGpuOcclusionPassDesc& desc);
        ~SceneGpuOcclusionPass();
        SceneGpuOcclusionPass(const SceneGpuOcclusionPass&) = delete;
        SceneGpuOcclusionPass& operator=(const SceneGpuOcclusionPass&) = delete;
        void PrepareFrame(const RenderCamera& rasterCamera, uint32_t drawCount,
            uint32_t batchCount, uint64_t completedSubmission);
        // Input bounds=float4(centerWS,radius); draws=uint4(object,batch,
        // visibleBase,padding), matching SceneGpuCull. Args are the existing
        // 20-byte indexed draw records. Reset touches InstanceCount only.
        void AddPasses(RenderGraph& graph, TextureHandle depth, uint32_t width, uint32_t height,
            BufferHandle bounds, BufferHandle drawData, BufferHandle visibleIndices, BufferHandle indirectArguments);
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Collect(uint64_t completedSubmission);
        void Clear(uint64_t completedSubmission);
        [[nodiscard]] std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;
    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
