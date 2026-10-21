#pragma once

#include "Runtime/Platform/Interop/EffekseerRenderInterop.h"

#include <memory>

namespace HEngine
{
    // Vulkan implementation of the backend-neutral Effekseer bridge. Native
    // Vulkan types stay inside its private implementation.
    class VulkanEffekseerInterop final : public IEffekseerRenderInterop
    {
    public:
        VulkanEffekseerInterop();
        ~VulkanEffekseerInterop() override;

        VulkanEffekseerInterop(const VulkanEffekseerInterop&) = delete;
        VulkanEffekseerInterop& operator=(const VulkanEffekseerInterop&) = delete;

        void Initialize(nvrhi::IDevice* device, const EffekseerRenderInteropDesc& desc) override;
        void Shutdown() noexcept override;

        void BeginHostFrame(uint32_t slot, uint64_t completed) override;
        void NotifyHostFrameSubmitted(uint32_t slot, uint64_t submission) noexcept override;
        void AbortHostFrame(uint32_t slot) noexcept override;
        void AbortRecording() noexcept override;

        void BeginFrame(const EffekseerRenderFrameDesc& frame) override;
        void BeginComputeFrame(nvrhi::ICommandList* commandList) override;
        void BeginRendering() override;
        void EndRendering() override;
        void EndFrame() override;

        [[nodiscard]] bool IsInitialized() const noexcept override;
        [[nodiscard]] EffekseerRenderer::Renderer* GetRenderer() const noexcept override;
        [[nodiscard]] const EffekseerRenderInteropStatus& GetStatus() const noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
