#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <string>

namespace HEngine
{
    struct TemporalUpscalerDesc
    {
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        uint32_t OutputWidth = 0;
        uint32_t OutputHeight = 0;
        bool bReversedDepth = false;
        bool bInfiniteDepth = false;
        bool bAutoExposure = true;
        bool bJitteredMotionVectors = false;
    };

    struct TemporalUpscalerFrame
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::ITexture* Color = nullptr;
        // Single-sample device depth in R32_FLOAT, including on Vulkan.
        nvrhi::ITexture* Depth = nullptr;
        // Current-to-previous motion; scale converts it to pixels. Jittered
        // inputs must opt in through TemporalUpscalerDesc.
        nvrhi::ITexture* MotionVectors = nullptr;
        nvrhi::ITexture* Exposure = nullptr;
        nvrhi::ITexture* Reactive = nullptr;
        nvrhi::ITexture* Composition = nullptr;
        nvrhi::ITexture* Output = nullptr;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        float JitterX = 0.0f;
        float JitterY = 0.0f;
        float MotionScaleX = 1.0f;
        float MotionScaleY = 1.0f;
        float FrameTimeMilliseconds = 16.667f;
        float PreExposure = 1.0f;
        float CameraNear = 0.1f;
        float CameraFar = 1000.0f;
        float VerticalFovRadians = 1.0472f;
        float Sharpness = 0.0f;
        bool bReset = true;
    };

    // The renderer owns synchronization: retain textures until submission is
    // complete and wait for outstanding dispatches before destroying a context.
    // The platform adapter restores the declared NVRHI resource states after
    // native dispatch and invalidates native pipeline/descriptor state caches.
    class ITemporalUpscaler
    {
    public:
        virtual ~ITemporalUpscaler() = default;
        virtual bool Dispatch(const TemporalUpscalerFrame& frame, std::string& error) = 0;
        virtual bool GetJitter(uint32_t frameIndex, uint32_t renderWidth,
            float& x, float& y, std::string& error) const = 0;
        [[nodiscard]] virtual const std::string& GetVersion() const noexcept = 0;
    };

    // A missing runtime or unsupported GPU returns null with a diagnostic;
    // callers must keep their native AA path available as a fallback.
    [[nodiscard]] std::unique_ptr<ITemporalUpscaler> CreateFsr31Upscaler(
        nvrhi::IDevice* device, const TemporalUpscalerDesc& desc, std::string& error);
}
