#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>

namespace HEngine
{
    class ShaderManager;

    struct SceneAntiAliasingPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
    };

    struct SceneTaaPassInputs
    {
        TextureHandle CurrentHdrColor;
        TextureHandle MotionVectors;
        TextureHandle CurrentDepth;
        TextureHandle ViewDepth;
        TextureHandle NormalRoughness;
        TextureHandle SurfaceId;
        // Optional full-resolution R16/R32_FLOAT confidence produced by an
        // already-temporal GI denoiser. When present, TAA uses a lower history
        // ceiling and treats low-confidence lighting as reactive.
        TextureHandle TemporalGiConfidence;
    };

    // Owns renderer-independent post-AA state. PrepareFrame is called once per
    // frame before graph execution; AddTaaPasses/AddFxaaPass/AddSmaaPasses are
    // called when the owning graph is built. UpdateImportedResources keeps a
    // compiled TAA graph pointed at the current ping-pong history textures.
    class SceneAntiAliasingPass final
    {
    public:
        explicit SceneAntiAliasingPass(const SceneAntiAliasingPassDesc& desc);
        ~SceneAntiAliasingPass();

        SceneAntiAliasingPass(const SceneAntiAliasingPass&) = delete;
        SceneAntiAliasingPass& operator=(const SceneAntiAliasingPass&) = delete;

        void PrepareFrame(
            const AntiAliasingSettings& settings,
            uint32_t width,
            uint32_t height,
            uint64_t completedSubmission,
            bool bCameraReset = false);
        void Resize(
            uint32_t width,
            uint32_t height,
            uint64_t completedSubmission);
        void ResetHistory() noexcept;
        // Restarts the finite jitter/convergence window while preserving valid
        // reprojection history (for ordinary camera/object motion).
        void RestartConvergence() noexcept;
        [[nodiscard]] uint32_t GetTemporalFrameIndex() const noexcept;
        [[nodiscard]] bool RequiresContinuousUpdate() const noexcept;

        // The five inputs and independent resolved output are single-sample.
        // Current color/history/resolved use RGBA16_FLOAT; guides use
        // RG16_FLOAT, D32/R32_FLOAT, RG32_FLOAT, RGBA16_FLOAT and RG32_UINT.
        // TemporalGiConfidence is optional and must be full-resolution. The
        // caller may composite later overlays into the resolved texture without
        // feeding those overlays back into temporal history.
        [[nodiscard]] TextureHandle AddTaaPasses(
            RenderGraph& renderGraph,
            const SceneTaaPassInputs& inputs);

        // FXAA consumes display-referred RGBA8_UNORM and fully overwrites the
        // caller-provided single-sample RGBA8_UNORM target.
        [[nodiscard]] TextureHandle AddFxaaPass(
            RenderGraph& renderGraph,
            TextureHandle ldrInput,
            TextureHandle ldrTarget,
            const AntiAliasingSettings& settings,
            uint32_t width,
            uint32_t height);

        // SMAA 1x consumes display-referred RGBA8_UNORM, creates transient
        // RG8 edge and RGBA8 blend-weight targets, then fully overwrites the
        // caller-provided single-sample RGBA8_UNORM target.
        [[nodiscard]] TextureHandle AddSmaaPasses(
            RenderGraph& renderGraph,
            TextureHandle ldrInput,
            TextureHandle ldrTarget,
            const AntiAliasingSettings& settings,
            uint32_t width,
            uint32_t height);

        void UpdateImportedResources(RenderGraph& renderGraph);
        [[nodiscard]] bool ConsumeGraphRebuildRequest() noexcept;
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Collect(uint64_t completedSubmission);
        void Clear(uint64_t completedSubmission);

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
