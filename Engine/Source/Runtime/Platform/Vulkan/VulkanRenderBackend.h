#pragma once

#include "Runtime/Graphics/RHI/RenderDevice.h"

namespace HEngine
{
    class VulkanRenderBackend final : public IRenderBackend, private nvrhi::IMessageCallback
    {
    public:
        VulkanRenderBackend();
        ~VulkanRenderBackend() override;

        VulkanRenderBackend(const VulkanRenderBackend&) = delete;
        VulkanRenderBackend& operator=(const VulkanRenderBackend&) = delete;

        void Initialize(
            RenderSurface& surface,
            bool bEnableValidation,
            const RenderAdapterSelectionOptions& selection,
            uint32_t framesInFlight) override;
        void Shutdown() override;
        [[nodiscard]] FrameContext& BeginFrame() override;
        void AbortFrame(FrameContext& frame) noexcept override;
        void SubmitAndPresent(FrameContext& frame, bool bVSync) override;
        void Resize(uint32_t width, uint32_t height) override;
        void WaitForIdle() override;

        [[nodiscard]] RenderBackend GetBackend() const override { return RenderBackend::Vulkan; }
        [[nodiscard]] const RenderCapabilities& GetCapabilities() const override { return mCapabilities; }
        [[nodiscard]] nvrhi::IDevice* GetDevice() const override;
        [[nodiscard]] SwapChain& GetSwapChain() override;

    private:
        struct Impl;
        void ReleaseSurface();
        void RestoreSurface();
        void message(nvrhi::MessageSeverity severity, const char* messageText) override;

        Scope<Impl> mImpl;
        RenderCapabilities mCapabilities;
    };
}
