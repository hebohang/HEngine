#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/RayTracing/RayTracingScene.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>

namespace HEngine
{
    class ShaderManager;

    struct SceneSurfaceDataPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        RayTracingScene* Scene = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    struct SceneSurfaceDataPassStats
    {
        uint32_t DrawCount = 0;
        uint32_t ExcludedInstanceCount = 0;
    };

    // Cornell-first raster primary-visibility contract. It re-rasterizes only
    // the static opaque geometry accepted by RayTracingScene and never mutates
    // SceneRenderer color/depth ownership.
    class SceneSurfaceDataPass final
    {
    public:
        explicit SceneSurfaceDataPass(const SceneSurfaceDataPassDesc& desc);
        ~SceneSurfaceDataPass();

        SceneSurfaceDataPass(const SceneSurfaceDataPass&) = delete;
        SceneSurfaceDataPass& operator=(const SceneSurfaceDataPass&) = delete;

        void PrepareFrame(
            const RenderCamera& camera,
            uint32_t width,
            uint32_t height);
        [[nodiscard]] SceneSurfaceData AddPasses(
            RenderGraph& renderGraph,
            TextureHandle sceneDepth,
            const RayTracingSceneGraphResources& sceneResources);

        [[nodiscard]] const SceneSurfaceDataPassStats& GetStats() const noexcept;
        void ClearCaches();
        [[nodiscard]] std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
