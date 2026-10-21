#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <nvrhi/nvrhi.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>

namespace HEngine
{
    class ShaderManager;

    struct ScenePostProcessSettings
    {
        // Main-game 3D resolution cap; the output and UI remain native sized.
        uint32_t RenderHeight = 0;
        float Exposure = 1.0f / 16384.0f;
        // Player calibration applied after camera/world exposure, in stops.
        float ExposureCompensationEV = 0.0f;
        float BloomIntensity = 0.05f;
        float BloomThreshold = 1.0f;
        bool bAutoExposure = false;
        float ExposureKey = 0.18f;
        float MinimumExposure = 1.0f / 1048576.0f;
        float MaximumExposure = 16.0f;
        float ExposureAdaptationSpeed = 2.0f;
        float Contrast = 1.0f;
        float Saturation = 1.0f;
        // Artistic warm/cool offset in [-1,1], not a color temperature in Kelvin.
        float Temperature = 0.0f;
        // Edge-aware base luminance compression in stops; zero preserves legacy rendering.
        float LocalExposureShadowStrength = 0.0f;
        float LocalExposureHighlightStrength = 0.0f;
        float LocalExposureMaxEV = 2.0f;
        // Deferred skin diffusion is scheduled before tone mapping and retains specular detail.
        bool SkinSubsurfaceEnabled = false;
        float SkinSubsurfaceRadiusMeters = 0.003f;
        int SkinSubsurfaceQuality = 1;
        float VignetteIntensity = 0.0f;
        // Per-channel grade in exposed linear space; neutral defaults preserve old scenes.
        glm::vec3 ColorGain{ 1.0f };
        glm::vec3 ColorOffset{ 0.0f };

        [[nodiscard]] bool operator==(const ScenePostProcessSettings&) const = default;
    };

    struct ScenePostProcessPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    // Per-view HDR post processing. Bloom and luminance use one quarter of
    // each native dimension. Exposure is a GPU-only scalar updated every frame.
    // PrepareFrame precedes execution, AddPasses is called only on graph build.
    class ScenePostProcessPass final
    {
    public:
        explicit ScenePostProcessPass(const ScenePostProcessPassDesc& desc);
        ~ScenePostProcessPass();
        ScenePostProcessPass(const ScenePostProcessPass&) = delete;
        ScenePostProcessPass& operator=(const ScenePostProcessPass&) = delete;

        void PrepareFrame(const ScenePostProcessSettings& settings, float deltaSeconds,
            float manualExposure, uint64_t completedSubmission, bool bResetHistory = false);
        [[nodiscard]] TextureHandle AddPasses(RenderGraph& graph,
            TextureHandle hdrColor, uint32_t width, uint32_t height);
        [[nodiscard]] bool ConsumeGraphRebuildRequest() noexcept;
        [[nodiscard]] bool RequiresContinuousUpdate() const noexcept;
        void ResetHistory() noexcept;
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
