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

    enum class SceneAmbientOcclusionTechnique : uint8_t { GTAO, SSAO };

    struct SceneAmbientOcclusionSettings
    {
        bool bEnabled = true;
        SceneAmbientOcclusionTechnique Technique = SceneAmbientOcclusionTechnique::GTAO;
        float Radius = 1.0f;
        float Intensity = 1.0f;
        float NormalBias = 0.01f;
        uint32_t SliceCount = 4;
        uint32_t StepsPerSide = 8;
        uint32_t SampleCount = 16;
        bool bFastUpsample = false;
        [[nodiscard]] bool operator==(const SceneAmbientOcclusionSettings&) const = default;
    };

    struct SceneAmbientOcclusionPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    // Raster depth is normal-Z [0,1]; packed normals are world-space octahedral
    // XY in [0,1], matching RasterSurfaceData. Outputs visibility (1 = unoccluded)
    // at native R8_UNORM after half-resolution horizon integration and bilateral
    // reconstruction. The caller applies this only to appropriate indirect light.
    class SceneAmbientOcclusionPass final
    {
    public:
        explicit SceneAmbientOcclusionPass(const SceneAmbientOcclusionPassDesc& desc);
        ~SceneAmbientOcclusionPass();
        SceneAmbientOcclusionPass(const SceneAmbientOcclusionPass&) = delete;
        SceneAmbientOcclusionPass& operator=(const SceneAmbientOcclusionPass&) = delete;
        void PrepareFrame(const SceneAmbientOcclusionSettings& settings, const RenderCamera& camera,
            uint32_t width, uint32_t height, uint64_t completedSubmission);
        // Returns an invalid handle when disabled; callers must use neutral AO.
        [[nodiscard]] TextureHandle AddPasses(RenderGraph& graph,
            TextureHandle depth, TextureHandle packedNormalRoughnessAo);
        [[nodiscard]] bool ConsumeGraphRebuildRequest() noexcept;
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Collect(uint64_t completedSubmission);
        void Clear(uint64_t completedSubmission);
        // Swap capsule for one prepared-graph transaction; retain it until the
        // old graph's last submission completes. Does not alter temporal data.
        [[nodiscard]] std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;
    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
