#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace HEngine
{
    // Native surface operations stay inside Platform. The renderer owns the
    // VkSurfaceKHR; the window must outlive it and the associated swap chain.
    class VulkanSurface
    {
    public:
        virtual ~VulkanSurface() = default;
        [[nodiscard]] virtual std::vector<const char*> GetVulkanInstanceExtensions() const = 0;
        [[nodiscard]] virtual VkResult CreateVulkanSurface(
            VkInstance instance, PFN_vkGetInstanceProcAddr getProcAddr,
            VkSurfaceKHR* surface) const = 0;
        virtual void GetFramebufferExtent(uint32_t& width, uint32_t& height) const = 0;
        [[nodiscard]] virtual bool GetSurfaceVSync() const = 0;
        // Called on the render thread before the OS releases its native window.
        // The backend unregisters its callback before destruction.
        virtual void SetSurfaceReleaseCallback(std::function<void()> callback) {}
    };
}
