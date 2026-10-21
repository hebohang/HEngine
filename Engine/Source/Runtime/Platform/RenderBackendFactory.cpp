#include "Hepch.h"

#include "Runtime/Graphics/RHI/RenderDevice.h"
#if HE_RENDER_DX12
#include "Runtime/Platform/DirectX/DX12/DX12Capabilities.h"
#include "Runtime/Platform/DirectX/DX12/DX12RenderBackend.h"
#endif
#if HE_RENDER_VULKAN
#include "Runtime/Platform/Vulkan/VulkanCapabilities.h"
#include "Runtime/Platform/Vulkan/VulkanRenderBackend.h"
#endif

#include <stdexcept>

namespace HEngine
{
    Scope<IRenderBackend> CreatePlatformRenderBackend(RenderBackend backend)
    {
        if (!IsRenderBackendCompiled(backend))
            throw std::runtime_error(GetRenderBackendAvailabilityReason(backend));

        switch (backend)
        {
#if HE_RENDER_DX12
        case RenderBackend::DX12:
            return CreateScope<DX12RenderBackend>();
#endif
#if HE_RENDER_VULKAN
        case RenderBackend::Vulkan:
            return CreateScope<VulkanRenderBackend>();
#endif
        default:
            break;
        }
        throw std::invalid_argument("Unknown render backend");
    }

    void RunRenderCapabilityProbe(
        RenderBackend backend,
        const std::filesystem::path& outputPath,
        const RenderAdapterSelectionOptions& selection)
    {
        if (!IsRenderBackendCompiled(backend))
            throw std::runtime_error(GetRenderBackendAvailabilityReason(backend));

        switch (backend)
        {
#if HE_RENDER_DX12
        case RenderBackend::DX12:
            RunDX12CapabilityProbe(outputPath, selection);
            return;
#endif
#if HE_RENDER_VULKAN
        case RenderBackend::Vulkan:
            RunVulkanCapabilityProbe(outputPath, selection);
            return;
#endif
        default:
            break;
        }
        throw std::invalid_argument("Unknown render backend");
    }
}
