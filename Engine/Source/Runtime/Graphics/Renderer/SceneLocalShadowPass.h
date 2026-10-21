#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace HEngine
{
    struct SceneLocalShadowPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        uint32_t Resolution = 512;
        uint32_t SpotViewBudget = 8;
        uint32_t PointLightBudget = 2;
        float DepthBias = 0.0015f;
        float NormalBias = 0.02f;
    };

    struct SceneLocalShadowView
    {
        glm::mat4 ViewProjection{ 1.0f };
        glm::vec3 Position{ 0.0f };
        std::array<glm::vec4, 6> FrustumPlanes{};
        std::vector<uint32_t> VisibleObjectGpuIndices;
        uint32_t Layer = 0;
        uint32_t Resolution = 512;
        uint32_t SourceLightIndex = 0;
        uint32_t PointFace = 0;
        bool bPointLight = false;

        [[nodiscard]] bool IsObjectVisible(uint32_t objectGpuIndex) const noexcept;
    };

    struct SceneLocalShadowCallbacks
    {
        // The caller declares its authoritative vertex/index/material/bone and
        // constant-buffer dependencies, then draws only IDs in each view.
        std::function<void(RenderGraphBuilder&)> DeclareCasterResources;
        std::function<void(RenderGraphContext&, const SceneLocalShadowView&,
            nvrhi::IFramebuffer*)> DrawView;
    };

    struct SceneLocalShadowGraphResources
    {
        TextureHandle DepthArray;
        BufferHandle ViewRecords;
    };

    struct SceneLocalShadowStats
    {
        uint32_t SelectedSpotLights = 0;
        uint32_t SelectedPointLights = 0;
        uint32_t UnshadowedCandidateLights = 0;
        uint32_t ActiveViews = 0;
        uint32_t DirtyViews = 0;
        uint32_t RenderedViews = 0;
        uint32_t VisibleCasterObjects = 0;
        uint64_t ResidentBytes = 0;
        uint64_t UploadedBytes = 0;
    };

    class SceneLocalShadowPass final
    {
    public:
        explicit SceneLocalShadowPass(const SceneLocalShadowPassDesc& desc);
        ~SceneLocalShadowPass();
        SceneLocalShadowPass(const SceneLocalShadowPass&) = delete;
        SceneLocalShadowPass& operator=(const SceneLocalShadowPass&) = delete;

        [[nodiscard]] bool PrepareFrame(const RenderWorld& world, uint64_t completedSubmission);
        [[nodiscard]] SceneLocalShadowGraphResources AddPasses(RenderGraph& graph,
            SceneLocalShadowCallbacks callbacks);
        [[nodiscard]] std::span<const uint32_t> GetPointShadowViews() const noexcept;
        [[nodiscard]] std::span<const uint32_t> GetSpotShadowViews() const noexcept;
        [[nodiscard]] std::span<const SceneLocalShadowView> GetViews() const noexcept;
        [[nodiscard]] std::span<const uint32_t> GetDirtyViewIndices() const noexcept;
        [[nodiscard]] nvrhi::ITexture* GetDepthArray() const noexcept;
        [[nodiscard]] nvrhi::IBuffer* GetViewRecordBuffer() const noexcept;
        [[nodiscard]] const SceneLocalShadowStats& GetStats() const noexcept;
        // The shared raster index selection changed after light/view prepare.
        // Refresh active views in this frame so a cached shadow never uses a
        // different authored LOD from its depth/lighting submissions.
        void InvalidateGeometry();
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
