#include "Hepch.h"

#include "RenderDevice.h"

#include <stdexcept>

namespace HEngine
{
    RenderDevice::~RenderDevice()
    {
        Shutdown();
    }

    void RenderDevice::Initialize(
        RenderBackend backend,
        RenderSurface& surface,
        bool bEnableValidation,
        const RenderAdapterSelectionOptions& selection)
    {
        if (mBackend)
            throw std::logic_error("RenderDevice is already initialized.");
        Scope<IRenderBackend> platformBackend = CreatePlatformRenderBackend(backend);
        platformBackend->Initialize(surface, bEnableValidation, selection);
        mBackend = std::move(platformBackend);
    }

    void RenderDevice::Shutdown()
    {
        if (!mBackend)
            return;

        mBackend->Shutdown();
        mBackend.reset();
    }

    FrameContext& RenderDevice::BeginFrame()
    {
        return RequireBackend().BeginFrame();
    }

    void RenderDevice::AbortFrame(FrameContext& frame) noexcept
    {
        if (mBackend)
            mBackend->AbortFrame(frame);
    }

    void RenderDevice::SubmitAndPresent(FrameContext& frame, bool bVSync)
    {
        RequireBackend().SubmitAndPresent(frame, bVSync);
    }

    void RenderDevice::Resize(uint32_t width, uint32_t height)
    {
        RequireBackend().Resize(width, height);
    }

    void RenderDevice::WaitForIdle()
    {
        RequireBackend().WaitForIdle();
    }

    RenderBackend RenderDevice::GetBackend() const
    {
        return RequireBackend().GetBackend();
    }

    const RenderCapabilities& RenderDevice::GetCapabilities() const
    {
        return RequireBackend().GetCapabilities();
    }

    nvrhi::IDevice* RenderDevice::GetDevice() const
    {
        return RequireBackend().GetDevice();
    }

    SwapChain& RenderDevice::GetSwapChain()
    {
        return RequireBackend().GetSwapChain();
    }

    IRenderBackend& RenderDevice::RequireBackend() const
    {
        if (!mBackend)
            throw std::logic_error("RenderDevice is not initialized.");
        return *mBackend;
    }
}
