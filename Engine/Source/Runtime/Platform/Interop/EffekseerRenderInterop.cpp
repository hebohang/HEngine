#include "Hepch.h"

#include "EffekseerRenderInterop.h"

#include "Runtime/Platform/Interop/DX12/DX12EffekseerInterop.h"
#include "Runtime/Platform/Interop/Vulkan/VulkanEffekseerInterop.h"

#include <stdexcept>

namespace HEngine
{
    std::unique_ptr<IEffekseerRenderInterop> CreateEffekseerRenderInterop(
        nvrhi::IDevice* device,
        const EffekseerRenderInteropDesc& desc)
    {
        if (device == nullptr)
            throw std::invalid_argument("Effekseer interop requires a valid NVRHI device");

        std::unique_ptr<IEffekseerRenderInterop> interop;
        switch (device->getGraphicsAPI())
        {
        case nvrhi::GraphicsAPI::D3D12:
            interop = std::make_unique<DX12EffekseerInterop>();
            break;
        case nvrhi::GraphicsAPI::VULKAN:
            interop = std::make_unique<VulkanEffekseerInterop>();
            break;
        default:
            throw std::invalid_argument("Effekseer interop supports only NVRHI D3D12 and Vulkan devices");
        }

        interop->Initialize(device, desc);
        return interop;
    }
}
