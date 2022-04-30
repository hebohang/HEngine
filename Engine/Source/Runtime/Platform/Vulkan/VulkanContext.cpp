#include "hepch.h"

#include "Runtime/Platform/Vulkan/VulkanContext.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace HEngine
{
    VulkanContext::VulkanContext(GLFWwindow* windowHandle)
        : mWindowHandle(windowHandle)
    {
        HE_CORE_ASSERT(windowHandle, "Window handle is null!");
    }

    void VulkanContext::Init()
    {
        glfwMakeContextCurrent(mWindowHandle);
    }

    void VulkanContext::SwapBuffers()
    {
        HE_PROFILE_FUNCTION();

        glfwSwapBuffers(mWindowHandle);
    }
}