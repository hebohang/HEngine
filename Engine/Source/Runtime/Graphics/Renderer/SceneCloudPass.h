#pragma once

#include "Runtime/Graphics/Renderer/SceneAmbientOcclusionPass.h"
#include "Runtime/Graphics/Renderer/VolumetricCloud.h"

namespace HEngine
{
    struct RenderWorld;
    class SceneCloudPass final
    {
    public:
        explicit SceneCloudPass(const SceneAmbientOcclusionPassDesc& desc);
        ~SceneCloudPass();
        void PrepareFrame(const CloudSettings& settings, const RenderWorld& world,
            const RenderCamera& rasterCamera, uint32_t width, uint32_t height, uint64_t completed);
        TextureHandle AddPasses(RenderGraph& graph, TextureHandle color, TextureHandle depth, TextureHandle rasterDepth);
        void UpdateImportedResources(RenderGraph& graph);
        bool ConsumeGraphRebuildRequest() noexcept;
        bool RequiresContinuousUpdate() const noexcept;
        void ResetHistory() noexcept;
        [[nodiscard]] double GetCpuRecordMilliseconds() const noexcept;
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Clear(uint64_t completed);
        void Collect(uint64_t completed);
        std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;
    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
