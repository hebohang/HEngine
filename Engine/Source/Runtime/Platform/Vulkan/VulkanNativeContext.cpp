#include "Hepch.h"

#include "VulkanNativeContext.h"

#include <mutex>

namespace HEngine
{
    namespace
    {
        std::mutex sContextMutex;
        VulkanNativeContext sContext;
    }

    const VulkanNativeContext* GetVulkanNativeContext(nvrhi::IDevice* device) noexcept
    {
        std::scoped_lock lock(sContextMutex);
        if (device == nullptr ||
            (device != sContext.NativeNvrhiDevice && device != sContext.PublicNvrhiDevice))
        {
            return nullptr;
        }
        return &sContext;
    }

    void RegisterVulkanNativeContext(const VulkanNativeContext& context)
    {
        std::scoped_lock lock(sContextMutex);
        if (sContext.NativeNvrhiDevice != nullptr)
            throw std::logic_error("Only one active Vulkan native context is supported");
        sContext = context;
    }

    void UnregisterVulkanNativeContext(nvrhi::IDevice* nativeDevice) noexcept
    {
        std::scoped_lock lock(sContextMutex);
        if (nativeDevice == sContext.NativeNvrhiDevice)
            sContext = {};
    }
}
