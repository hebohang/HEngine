#pragma once

#include "Runtime/Core/Window/Window.h"
#include "Runtime/Graphics/RHI/RenderSurface.h"
#include "Runtime/Platform/Vulkan/VulkanSurface.h"

#include <android/native_window.h>

namespace HEngine
{
    class AndroidWindow final : public Window, public RenderSurface, public VulkanSurface
    {
    public:
        explicit AndroidWindow(const WindowProps& props);
        ~AndroidWindow() override;
        void OnUpdate() override {}
        bool WaitForFrame() override;
        bool ConsumeResume() override;
        uint32_t GetWidth() const override { return mWidth; }
        uint32_t GetHeight() const override { return mHeight; }
        void SetClientSize(uint32_t, uint32_t) override;
        void SetEventCallback(const EventCallbackFn& callback) override { mCallback = callback; }
        void SetVSync(bool) override {}
        bool IsVSync() const override { return true; }
        bool IsFullscreen() const override { return true; }
        std::vector<const char*> GetVulkanInstanceExtensions() const override;
        VkResult CreateVulkanSurface(VkInstance instance, PFN_vkGetInstanceProcAddr getProcAddr,
            VkSurfaceKHR* surface) const override;
        void GetFramebufferExtent(uint32_t& width, uint32_t& height) const override;
        bool GetSurfaceVSync() const override { return true; }
        void SetSurfaceReleaseCallback(std::function<void()> callback) override { mRelease = std::move(callback); }
    private:
        void PumpEvents();
        ANativeWindow* mWindow = nullptr;
        uint32_t mWidth = 0, mHeight = 0;
        bool bResumed = false, bPrimary = false, bResumePending = false, bAudioSuspended = false;
        EventCallbackFn mCallback;
        std::function<void()> mRelease;
    };
}
