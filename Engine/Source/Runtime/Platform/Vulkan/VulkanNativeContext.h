#pragma once

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace HEngine
{
    // Platform-only native context for Vulkan integrations such as Effekseer.
    // It is valid only while the owning VulkanRenderBackend is initialized.
    struct VulkanNativeContext
    {
        VkInstance Instance = VK_NULL_HANDLE;
        VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
        VkDevice Device = VK_NULL_HANDLE;
        VkQueue GraphicsQueue = VK_NULL_HANDLE;
        uint32_t GraphicsQueueFamilyIndex = 0;
        VkCommandPool InteropCommandPool = VK_NULL_HANDLE;
        nvrhi::IDevice* NativeNvrhiDevice = nullptr;
        nvrhi::IDevice* PublicNvrhiDevice = nullptr;
    };

    [[nodiscard]] const VulkanNativeContext* GetVulkanNativeContext(
        nvrhi::IDevice* device) noexcept;

    // Backend-owned registration. These are intentionally unavailable from
    // Graphics/RHI so native Vulkan ownership remains under Runtime/Platform.
    void RegisterVulkanNativeContext(const VulkanNativeContext& context);
    void UnregisterVulkanNativeContext(nvrhi::IDevice* nativeDevice) noexcept;
}
