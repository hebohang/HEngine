#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>

namespace EffekseerRenderer
{
    class Renderer;
}

namespace HEngine
{
    struct EffekseerDepthReconstruction
    {
        float DepthBufferScale = 1.0f;
        float DepthBufferOffset = 0.0f;
        float ProjectionMatrix33 = 0.0f;
        float ProjectionMatrix34 = 0.0f;
        float ProjectionMatrix43 = 0.0f;
        float ProjectionMatrix44 = 0.0f;
    };

    struct EffekseerRenderInteropDesc
    {
        nvrhi::Format ColorFormat = nvrhi::Format::UNKNOWN;
        nvrhi::Format DepthFormat = nvrhi::Format::UNKNOWN;
        uint32_t FramesInFlight = 3;
        int32_t MaxSpriteCount = 8192;
        bool bReversedDepth = false;
    };

    struct EffekseerRenderFrameDesc
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::IFramebuffer* Framebuffer = nullptr;

        // Supply a synchronized, single-sample R32_FLOAT depth snapshot.
        // Framebuffer's depth attachment remains read-only for depth testing.
        nvrhi::ITexture* DepthSampleTexture = nullptr;
        nvrhi::ITexture* BackgroundTexture = nullptr;
        EffekseerDepthReconstruction DepthReconstruction;
    };

    struct EffekseerRenderInteropStatus
    {
        bool bInitialized = false;
        bool bFrameActive = false;
        bool bRendering = false;
        bool bDepthSampleBound = false;
        bool bBackgroundBound = false;
    };

    // Backend-neutral boundary used by Graphics/VFX. Native API handles and
    // renderer-specific setup remain in Runtime/Platform/Interop implementations.
    class IEffekseerRenderInterop
    {
    public:
        virtual ~IEffekseerRenderInterop() = default;

        virtual void Initialize(nvrhi::IDevice* device, const EffekseerRenderInteropDesc& desc) = 0;
        virtual void Shutdown() noexcept = 0;

        virtual void BeginFrame(const EffekseerRenderFrameDesc& frame) = 0;
        virtual void BeginRendering() = 0;
        virtual void EndRendering() = 0;
        virtual void EndFrame() = 0;

        [[nodiscard]] virtual bool IsInitialized() const noexcept = 0;
        [[nodiscard]] virtual EffekseerRenderer::Renderer* GetRenderer() const noexcept = 0;
        [[nodiscard]] virtual const EffekseerRenderInteropStatus& GetStatus() const noexcept = 0;
    };

    [[nodiscard]] std::unique_ptr<IEffekseerRenderInterop> CreateEffekseerRenderInterop(
        nvrhi::IDevice* device,
        const EffekseerRenderInteropDesc& desc);
}
