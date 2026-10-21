#pragma once

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace HEngine
{
    struct EffekseerRenderGraphInputs
    {
        TextureHandle SceneColor;
        TextureHandle SceneDepth;
        uint32_t Width = 0;
        uint32_t Height = 0;
        glm::mat4 View{ 1.0f };
        glm::mat4 Projection{ 1.0f };
        glm::vec3 CameraPosition{ 0.0f };
        float ZNear = 0.1f;
        float ZFar = 1000.0f;
        glm::vec3 LightDirection{ 0.0f, 1.0f, 0.0f };
        glm::vec3 LightColor{ 1.0f };
        glm::vec3 LightAmbientColor{ 0.25f };
    };

    struct EffekseerRenderGraphPassResult
    {
        RenderGraphPassHandle DepthSnapshotPass;
        RenderGraphPassHandle BackgroundCopyPass;
        RenderGraphPassHandle VfxPass;
        TextureHandle DepthSnapshot;
        TextureHandle BackgroundSnapshot;
        bool bAdded = false;
        bool bDepthSnapshotAdded = false;
        bool bBackgroundCopyAdded = false;
    };

    struct EffekseerRenderGraphPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        std::filesystem::path ShaderCacheDirectory;
    };

    struct EffekseerRenderView
    {
        glm::mat4 View{ 1.0f };
        glm::mat4 Projection{ 1.0f };
        glm::vec3 CameraPosition{ 0.0f };
        float ZNear = 0.1f;
        float ZFar = 1000.0f;
        glm::vec3 LightDirection{ 0.0f, 1.0f, 0.0f };
        glm::vec3 LightColor{ 1.0f };
        glm::vec3 LightAmbientColor{ 0.25f };
    };

    // Owns the immutable depth-conversion state and resource-dependent views
    // used by the Effekseer RenderGraph passes. The caller owns this object for
    // at least as long as every graph containing one of its execute callbacks.
    class EffekseerRenderGraphPass final
    {
    public:
        static void AddSimulationToGraph(RenderGraph& graph);
        explicit EffekseerRenderGraphPass(const EffekseerRenderGraphPassDesc& desc);
        ~EffekseerRenderGraphPass();

        EffekseerRenderGraphPass(const EffekseerRenderGraphPass&) = delete;
        EffekseerRenderGraphPass& operator=(const EffekseerRenderGraphPass&) = delete;

        [[nodiscard]] EffekseerRenderGraphPassResult AddToGraph(
            RenderGraph& graph,
            const EffekseerRenderGraphInputs& inputs,
            const VFXRenderRequirements& requirements,
            bool bRunEmptyPass = false);

        // Updates frame-varying camera data without rebuilding the otherwise
        // static graph or making execute callbacks capture a frame-local world.
        void UpdateView(const EffekseerRenderView& view);

        // Call after the owning queue is idle when graph resources are resized
        // or destroyed. Immutable shaders/layout/pipeline remain cached.
        void ClearResourceCaches();

        // Exchanges only graph resource views; the live VFX manager and frame
        // camera are independent of candidate graph preparation.
        [[nodiscard]] std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;

        [[nodiscard]] size_t GetFramebufferCacheSize() const noexcept;
        [[nodiscard]] size_t GetDepthBindingSetCacheSize() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
