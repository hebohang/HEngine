#pragma once

#include "Runtime/Platform/Interop/EffekseerRenderInterop.h"

#include <memory>

namespace HEngine
{
    using DX12EffekseerDepthReconstruction = EffekseerDepthReconstruction;
    using DX12EffekseerInteropDesc = EffekseerRenderInteropDesc;
    using DX12EffekseerFrameDesc = EffekseerRenderFrameDesc;
    using DX12EffekseerInteropStatus = EffekseerRenderInteropStatus;

    // Adapts Effekseer's DX12 renderer to an already-open NVRHI graphics
    // command list. It never creates a D3D12 device or queue. The owner must
    // wait for GPU idle before Shutdown releases Effekseer-owned resources.
    class DX12EffekseerInterop final : public IEffekseerRenderInterop
    {
    public:
        DX12EffekseerInterop();
        ~DX12EffekseerInterop() override;

        DX12EffekseerInterop(const DX12EffekseerInterop&) = delete;
        DX12EffekseerInterop& operator=(const DX12EffekseerInterop&) = delete;

        void Initialize(nvrhi::IDevice* device, const DX12EffekseerInteropDesc& desc) override;
        void Shutdown() noexcept override;

        void BeginHostFrame(uint32_t slot, uint64_t completed) override;
        void NotifyHostFrameSubmitted(uint32_t slot, uint64_t submission) noexcept override;
        void AbortHostFrame(uint32_t slot) noexcept override;
        void AbortRecording() noexcept override;

        void BeginFrame(const DX12EffekseerFrameDesc& frame) override;
        void BeginComputeFrame(nvrhi::ICommandList* commandList) override;
        void BeginRendering() override;
        void EndRendering() override;
        void EndFrame() override;

        [[nodiscard]] bool IsInitialized() const noexcept override;
        [[nodiscard]] EffekseerRenderer::Renderer* GetRenderer() const noexcept override;
        [[nodiscard]] const DX12EffekseerInteropStatus& GetStatus() const noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
