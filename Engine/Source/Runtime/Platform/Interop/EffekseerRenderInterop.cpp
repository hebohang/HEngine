#include "Hepch.h"

#include "EffekseerRenderInterop.h"

#if HE_RENDER_DX12
#include "Runtime/Platform/Interop/DX12/DX12EffekseerInterop.h"
#endif
#if HE_RENDER_VULKAN
#include "Runtime/Platform/Interop/Vulkan/VulkanEffekseerInterop.h"
#endif

#include <EffekseerRendererCommon/EffekseerRenderer.Renderer.h>
#include <EffekseerRendererLLGI/BufferPoolStats.h>

#include <stdexcept>

namespace HEngine
{
    EffekseerBufferPoolStats IEffekseerRenderInterop::GetBufferPoolStats() const
    {
        EffekseerBufferPoolStats stats;
        const auto* renderer = GetRenderer();
        if (!renderer)
            return stats;
        const auto graphicsDevice = renderer->GetGraphicsDevice();
        const auto pool = EffekseerRendererLLGI::Backend::GetBufferPoolStats(graphicsDevice.Get());
        stats.bAvailable = pool.Available;
        stats.StorageBufferCount = pool.StorageBufferCount;
        stats.StorageLogicalBytes = pool.StorageLogicalBytes;
        stats.UniformBufferCount = pool.UniformBufferCount;
        stats.UniformLogicalBytes = pool.UniformLogicalBytes;
        stats.UniformSnapshotCount = pool.UniformSnapshotCount;
        return stats;
    }

    std::unique_ptr<IEffekseerRenderInterop> CreateEffekseerRenderInterop(
        nvrhi::IDevice* device,
        const EffekseerRenderInteropDesc& desc)
    {
        if (device == nullptr)
            throw std::invalid_argument("Effekseer interop requires a valid NVRHI device");

        std::unique_ptr<IEffekseerRenderInterop> interop;
        switch (device->getGraphicsAPI())
        {
#if HE_RENDER_DX12
        case nvrhi::GraphicsAPI::D3D12:
            interop = std::make_unique<DX12EffekseerInterop>();
            break;
#endif
#if HE_RENDER_VULKAN
        case nvrhi::GraphicsAPI::VULKAN:
            interop = std::make_unique<VulkanEffekseerInterop>();
            break;
#endif
        default:
            throw std::invalid_argument("Effekseer interop supports only NVRHI D3D12 and Vulkan devices");
        }

        interop->Initialize(device, desc);
        return interop;
    }
}
