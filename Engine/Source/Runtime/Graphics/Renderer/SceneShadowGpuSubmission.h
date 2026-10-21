#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"
#include "Runtime/Graphics/Renderer/SceneIndirectCompactionPass.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace HEngine
{
    class ShaderManager;

    struct SceneShadowGpuSubmissionDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    struct SceneShadowGpuDraw
    {
        // ObjectIndex addresses the renderer's existing GPU object/bounds row,
        // not the stable entity identifier stored inside that row.
        uint32_t ObjectIndex = 0;
        uint32_t BatchIndex = 0;
        uint32_t VisibleBase = 0;
        uint32_t Padding = 0;
    };

    struct SceneShadowGpuViewResources
    {
        BufferHandle VisibleObjects;
        BufferHandle IndirectArguments;
        BufferHandle CompactedArguments;
    };

    struct SceneShadowGpuSubmissionStats
    {
        uint32_t CandidateDraws = 0;
        uint32_t BatchCount = 0;
        uint32_t CulledViews = 0;
        uint32_t CompactedViews = 0;
        uint32_t GroupCount = 0;
        uint64_t UploadedBytes = 0;
        uint64_t ResidentBytes = 0;
        uint64_t RetiredBytes = 0;
    };

    // Geometry and material tables remain owned by SceneRenderer. This module
    // only builds per-view lists and arguments for compatible static batches.
    class SceneShadowGpuSubmission final
    {
    public:
        explicit SceneShadowGpuSubmission(const SceneShadowGpuSubmissionDesc& desc);
        ~SceneShadowGpuSubmission();
        SceneShadowGpuSubmission(const SceneShadowGpuSubmission&) = delete;
        SceneShadowGpuSubmission& operator=(const SceneShadowGpuSubmission&) = delete;

        // Templates must have InstanceCount=0 and StartInstanceLocation equal
        // to their batch VisibleBase in the per-instance vertex stream. Each
        // batch owns a disjoint candidate-sized range in the visible list.
        [[nodiscard]] bool PrepareFrame(std::span<const SceneShadowGpuDraw> draws,
            std::span<const nvrhi::DrawIndexedIndirectArguments> arguments,
            uint32_t viewCount, uint64_t completedSubmission,
            std::span<const SceneIndirectGroupMapping> groups, uint32_t groupCount);
        void SetView(uint32_t viewIndex, const glm::mat4& viewProjection, bool bEnabled = true);
        [[nodiscard]] std::vector<SceneShadowGpuViewResources> AddPasses(
            RenderGraph& graph, BufferHandle authoritativeBounds, bool bCompact);
        [[nodiscard]] nvrhi::IBuffer* GetVisibleBuffer(uint32_t viewIndex) const;
        [[nodiscard]] nvrhi::IBuffer* GetIndirectBuffer(uint32_t viewIndex) const;
        [[nodiscard]] nvrhi::IBuffer* GetCompactedBuffer(uint32_t viewIndex) const;
        [[nodiscard]] uint32_t GetCountOffset(uint32_t groupIndex) const;
        [[nodiscard]] const SceneShadowGpuSubmissionStats& GetStats() const noexcept;
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Collect(uint64_t completedSubmission);
        [[nodiscard]] std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
