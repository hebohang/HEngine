#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

namespace HEngine
{
    class ShaderManager;
    struct RenderCamera;

    struct SceneReflectionSettings
    {
        bool bScreenSpaceEnabled = true;
        bool bLocalProbesEnabled = true;
        bool bGuideCacheEnabled = false;
        float MaximumDistance = 50.0f;
        float Thickness = 0.2f;
        float RoughnessCutoff = 0.6f;
        float EdgeFade = 0.08f;
        float NormalBias = 0.03f;
        uint32_t StepCount = 64;
        uint32_t BinaryRefinementSteps = 5;
        [[nodiscard]] bool operator==(const SceneReflectionSettings&) const = default;
    };

    // Cubemaps are prefiltered by the existing environment cache. Bounds are a
    // world-space axis-aligned influence/parallax box; positions inside overlapping
    // boxes blend by distance to their boundary. The caller supplies a stable
    // selection of at most MaximumSceneReflectionProbes probes for each view.
    inline constexpr uint32_t MaximumSceneReflectionProbes = 8;
    struct SceneReflectionProbe
    {
        glm::vec3 BoundsMin{ -5.0f };
        float BlendDistance = 1.0f;
        glm::vec3 BoundsMax{ 5.0f };
        float Intensity = 1.0f;
        glm::vec3 CapturePosition{ 0.0f };
        nvrhi::TextureHandle PrefilteredEnvironment;
        nvrhi::TextureHandle Irradiance;
    };
    struct SceneReflectionPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    class SceneReflectionPass final
    {
    public:
        explicit SceneReflectionPass(const SceneReflectionPassDesc& desc);
        ~SceneReflectionPass();
        SceneReflectionPass(const SceneReflectionPass&) = delete;
        SceneReflectionPass& operator=(const SceneReflectionPass&) = delete;
        // Camera must match the jittered raster camera that produced the depth.
        // globalMaximumMip is mipLevels - 1, identical to the opaque IBL shader.
        void PrepareFrame(const SceneReflectionSettings& settings, const RenderCamera& camera,
            uint32_t width, uint32_t height, float iblIntensity, float globalMaximumMip,
            std::span<const SceneReflectionProbe> probes, uint64_t completedSubmission,
            float diffuseIblWeight = 1.0f);
        // Half-resolution SSR, then guided native composition. Replaces the
        // existing global IBL specular, preserving direct light/GI/emissive.
        // screenSpaceAo must be neutral white when GTAO is disabled.
        [[nodiscard]] TextureHandle AddPasses(RenderGraph& graph, TextureHandle opaqueHdr,
            TextureHandle depth, TextureHandle baseColorMetallic, TextureHandle normalRoughnessAo,
            TextureHandle emissiveModel, TextureHandle screenSpaceAo,
            TextureHandle globalPrefilteredEnvironment, TextureHandle brdfLut,
            TextureHandle globalIrradiance);
        [[nodiscard]] bool ConsumeGraphRebuildRequest() noexcept;
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
