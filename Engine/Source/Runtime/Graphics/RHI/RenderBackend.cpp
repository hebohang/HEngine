#include "Hepch.h"

#include "RenderBackend.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace HEngine
{
#ifndef HE_RENDER_DX12
#define HE_RENDER_DX12 1
#endif
#ifndef HE_RENDER_VULKAN
#define HE_RENDER_VULKAN 0
#endif

    const char* ToString(RenderBackend backend)
    {
        switch (backend)
        {
        case RenderBackend::DX12:   return "DX12";
        case RenderBackend::Vulkan: return "Vulkan";
        }

        return "Unknown";
    }

    RenderBackend ParseRenderBackend(std::string_view name)
    {
        std::string normalized(name);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::toupper(character));
        });

        if (normalized == "DX12")
            return RenderBackend::DX12;
        if (normalized == "VULKAN")
            return RenderBackend::Vulkan;

        throw std::invalid_argument(
            "Unsupported render backend '" + std::string(name) + "'. Expected DX12 or Vulkan.");
    }

    bool IsRenderBackendCompiled(RenderBackend backend) noexcept
    {
        switch (backend)
        {
        case RenderBackend::DX12:   return HE_RENDER_DX12 != 0;
        case RenderBackend::Vulkan: return HE_RENDER_VULKAN != 0;
        }
        return false;
    }

    std::string GetRenderBackendAvailabilityReason(RenderBackend backend)
    {
        if (IsRenderBackendCompiled(backend))
        {
            return std::string(ToString(backend)) +
                " is compiled into this executable; device and driver availability is checked at initialization.";
        }
        return std::string(ToString(backend)) +
            " is not compiled into this executable. Reconfigure with the corresponding HE_RENDER_* option enabled.";
    }

    const char* ToString(RendererProfile profile)
    {
        switch (profile)
        {
        case RendererProfile::BaselineRaster: return "Baseline Raster";
        case RendererProfile::ModernRaster:   return "Modern Raster";
        case RendererProfile::HardwareRayTracing: return "Hardware Ray Tracing";
        }

        return "Unknown";
    }

    void ResolveRendererCapabilityPolicy(
        RenderCapabilities& capabilities,
        bool bHardwareRayTracingRendererRegistered)
    {
        capabilities.bHardwareRayTracingRendererReady =
            capabilities.RayTracingAccelStruct.bEnabled &&
            capabilities.RayTracingPipeline.bEnabled;
        capabilities.HardwareRayTracingRendererPath.bSupported =
            capabilities.bHardwareRayTracingRendererReady;
        capabilities.HardwareRayTracingRendererPath.bEnabled =
            capabilities.bHardwareRayTracingRendererReady &&
            bHardwareRayTracingRendererRegistered;
        if (capabilities.HardwareRayTracingRendererPath.bEnabled)
        {
            capabilities.HardwareRayTracingRendererPath.Reason =
                "Hardware ray-tracing acceleration structures and pipelines are ready and a renderer technique is registered.";
            capabilities.Profile = RendererProfile::HardwareRayTracing;
            return;
        }
        capabilities.HardwareRayTracingRendererPath.Reason =
            capabilities.bHardwareRayTracingRendererReady
            ? "Hardware ray-tracing acceleration structures and pipelines are ready, but no renderer technique is registered in this build; raster rendering remains selected."
            : "Hardware ray-tracing acceleration structures and pipelines are not both available; raster rendering remains selected.";

        if (capabilities.ShaderModel6_6.bEnabled ||
            capabilities.ResourceBindingTier3.bEnabled ||
            capabilities.HeapDirectlyIndexed.bEnabled ||
            capabilities.EnhancedBarriers.bEnabled ||
            capabilities.MeshShader.bEnabled ||
            capabilities.VariableRateShading.bEnabled)
        {
            capabilities.Profile = RendererProfile::ModernRaster;
        }
        else
        {
            capabilities.Profile = RendererProfile::BaselineRaster;
        }
    }
}
