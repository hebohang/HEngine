#pragma once

#include "Runtime/Graphics/Renderer/SceneAntiAliasingPass.h"
#include "Runtime/Graphics/Renderer/Upscaling.h"
#include "Runtime/Graphics/RHI/TemporalUpscaler.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

namespace HEngine
{
    class SceneUpscalingPass final
    {
    public:
        explicit SceneUpscalingPass(const SceneAntiAliasingPassDesc& desc);
        ~SceneUpscalingPass();
        RenderExtent Prepare(const UpscalingSettings& settings, RenderExtent output,
            RenderExtent fallback, bool eligible, uint64_t completed);
        glm::vec2 PrepareFrame(const RenderCamera& camera, double timeSeconds, bool reset);
        void BeginTransparency(RenderGraph& graph, TextureHandle color);
        TextureHandle AddPasses(RenderGraph& graph, TextureHandle color,
            TextureHandle opaque, TextureHandle depth, TextureHandle motion);
        bool IsActive() const noexcept;
        bool HasFailed() const noexcept;
        UpscalingStatus GetStatus() const noexcept;
        const std::string& GetReason() const noexcept;
        Json GetDiagnostics() const;
        TextureHandle GetReactiveMask() const noexcept;
        TextureHandle GetCompositionMask() const noexcept;
        TextureHandle GetMotionVectors() const noexcept;
        TextureHandle GetInputColor() const noexcept;
        TextureHandle GetOpaqueColor() const noexcept;
#if defined(HE_DEV_AGENT_RUNTIME)
        bool InjectFailureForValidation(bool duringDispatch) noexcept;
#endif
        void ResetHistory() noexcept;
        void NotifySubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Clear(); // caller has waited for the GPU
    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
