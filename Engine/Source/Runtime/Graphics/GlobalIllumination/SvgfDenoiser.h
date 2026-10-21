#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>

namespace HEngine
{
    class ShaderManager;

    struct SvgfDenoiserDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    class SvgfDenoiser final : public IDenoiser
    {
    public:
        explicit SvgfDenoiser(const SvgfDenoiserDesc& desc);
        ~SvgfDenoiser() override;
        [[nodiscard]] uint32_t GetMinimumRayCount() const noexcept override;
        void SetReactiveRegion(SsgiReactiveRegion region) override;
        [[nodiscard]] SsgiReactiveRegion GetReactiveRegion() const noexcept override;
        [[nodiscard]] DenoiserBudgetDiagnostics GetBudgetDiagnostics() const noexcept override;

        SvgfDenoiser(const SvgfDenoiser&) = delete;
        SvgfDenoiser& operator=(const SvgfDenoiser&) = delete;

        void PrepareFrame(
            const GlobalIlluminationHistorySignature& signature,
            uint64_t completedSubmission) override;
        [[nodiscard]] GlobalIlluminationOutputs AddPasses(
            RenderGraph& renderGraph,
            const GlobalIlluminationNoisySignals& noisySignals,
            const GlobalIlluminationDenoiserGuides& guides,
            const GlobalIlluminationSettings& settings) override;
        void ResetHistory(GlobalIlluminationHistoryResetReason reason) override;
        void UpdateImportedResources(RenderGraph& renderGraph) override;
        [[nodiscard]] std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;
        void NotifyFrameSubmitted(uint64_t submission) noexcept override;
        void AbortFrame() noexcept override;

        [[nodiscard]] bool ConsumeGraphRebuildRequest() noexcept override;
        void Collect(uint64_t completedSubmission) override;
        void Clear(uint64_t completedSubmission) override;

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
