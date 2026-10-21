#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>

namespace HEngine
{
    class ShaderManager;

    struct ScreenSpaceGlobalIlluminationDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        IDenoiser* Denoiser = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        uint32_t FramesInFlight = 1;
    };

    struct ScreenSpaceGlobalIlluminationFrameParameters
    {
        RenderCamera Camera;
        GlobalIlluminationHistorySignature HistorySignature;
        SsgiReactiveRegion ReactiveRegion;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSequenceIndex = 0;
        uint32_t DepthMipCount = 1;
        uint32_t EmissiveTriangleCount = 0;
        uint64_t CompletedSubmission = 0;
    };

    class ScreenSpaceGlobalIllumination final
        : public IGlobalIlluminationTechnique
    {
    public:
        explicit ScreenSpaceGlobalIllumination(
            const ScreenSpaceGlobalIlluminationDesc& desc);
        ~ScreenSpaceGlobalIllumination() override;

        ScreenSpaceGlobalIllumination(
            const ScreenSpaceGlobalIllumination&) = delete;
        ScreenSpaceGlobalIllumination& operator=(
            const ScreenSpaceGlobalIllumination&) = delete;

        void PrepareFrame(
            const ScreenSpaceGlobalIlluminationFrameParameters& frame,
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

        [[nodiscard]] const GlobalIlluminationOutputs&
            GetLastRawOutputs() const noexcept;
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
