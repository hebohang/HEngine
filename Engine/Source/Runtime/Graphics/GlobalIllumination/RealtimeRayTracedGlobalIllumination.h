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

    struct RealtimeRtgiDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        RayTracingScene* Scene = nullptr;
        IDenoiser* Denoiser = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    struct RealtimeRtgiFrameParameters
    {
        RenderCamera Camera;
        GlobalIlluminationHistorySignature HistorySignature;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSequenceIndex = 0;
        uint32_t FixedSeed = 0x52544749u;
        float MaximumRayDistance = 1000.0f;
        glm::vec3 EnvironmentRadiance{ 0.0f };
        uint64_t CompletedSubmission = 0;
    };

    class RealtimeRayTracedGlobalIllumination final
        : public IGlobalIlluminationTechnique
    {
    public:
        explicit RealtimeRayTracedGlobalIllumination(const RealtimeRtgiDesc& desc);
        ~RealtimeRayTracedGlobalIllumination() override;

        RealtimeRayTracedGlobalIllumination(
            const RealtimeRayTracedGlobalIllumination&) = delete;
        RealtimeRayTracedGlobalIllumination& operator=(
            const RealtimeRayTracedGlobalIllumination&) = delete;

        void PrepareFrame(
            const RealtimeRtgiFrameParameters& frame,
            const GlobalIlluminationSettings& settings);

        [[nodiscard]] bool IsSupported(
            const RenderCapabilities& capabilities) const override;
        [[nodiscard]] GlobalIlluminationPrepareResult PrepareFrame(
            const GlobalIlluminationFrameContext& frame,
            const GlobalIlluminationSettings& settings) override;
        [[nodiscard]] GlobalIlluminationOutputs AddPasses(
            RenderGraph& renderGraph,
            const GlobalIlluminationInputs& inputs,
            const GlobalIlluminationSettings& settings) override;
        void ResetHistory(GlobalIlluminationHistoryResetReason reason) override;
        [[nodiscard]] bool RequiresContinuousUpdate() const noexcept override;
        [[nodiscard]] GlobalIlluminationStats GetStats() const override;
        [[nodiscard]] const GlobalIlluminationOutputs& GetLastRawOutputs() const noexcept;

        void NotifyFrameSubmitted(uint64_t submission) noexcept override;
        void UpdateImportedResources(RenderGraph& renderGraph) override;
        [[nodiscard]] std::function<void()> CaptureGraphState() override;
        void ClearGraphResourceCaches() noexcept override;
        void AbortFrame() noexcept override;
        void Collect(uint64_t completedSubmission) override;
        void Clear(uint64_t completedSubmission) override;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
