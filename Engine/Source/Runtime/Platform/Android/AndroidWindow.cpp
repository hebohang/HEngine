#include "AndroidWindow.h"
#include "AndroidContext.h"
#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Events/ApplicationEvent.h"
#include "Runtime/Events/MouseEvent.h"

#include <vulkan/vulkan_android.h>

#include <stdexcept>
#include <utility>

namespace HEngine
{
    Scope<Window> Window::Create(const WindowProps& props) { return CreateScope<AndroidWindow>(props); }
    AndroidWindow::AndroidWindow(const WindowProps&)
    {
        if (!WaitForFrame()) throw std::runtime_error("Android stopped before its window was created");
    }
    AndroidWindow::~AndroidWindow()
    {
        if (mWindow) ANativeWindow_release(mWindow);
    }
    void AndroidWindow::SetClientSize(uint32_t, uint32_t)
    {
        throw std::logic_error("Android window size is controlled by the Activity");
    }
    void AndroidWindow::PumpEvents()
    {
        auto& state = GetAndroidContext();
        std::unique_lock lock(state.Mutex);
        if (state.WindowRevision != state.AppliedRevision)
        {
            const auto revision = state.WindowRevision;
            auto* next = std::exchange(state.PendingWindow, nullptr);
            lock.unlock();
            try { if (mRelease) mRelease(); }
            catch (...) { if (next) ANativeWindow_release(next); throw; }
            if (mWindow) ANativeWindow_release(mWindow);
            mWindow = next;
            bResumePending = true;
            lock.lock();
            state.AppliedRevision = revision;
            state.Changed.notify_all();
        }
        if (bResumed != state.Resumed)
        {
            bResumePending = state.Resumed;
            bResumed = state.Resumed;
        }
        auto inputs = std::move(state.InputEvents);
        state.InputEvents.clear();
        inputs.push_back(state.PendingInput);
        lock.unlock();
        uint32_t width = 0, height = 0;
        GetFramebufferExtent(width, height);
        if (width != mWidth || height != mHeight)
        {
            mWidth = width; mHeight = height;
            if (mCallback) { WindowResizeEvent event(width, height); mCallback(event); }
        }
        bool jump = false, pause = false;
        for (const auto& input : inputs)
        {
            { std::lock_guard inputLock(state.Mutex); state.Input = input; }
            jump |= input.Jump; pause |= input.Pause;
            if (mCallback)
            {
                MouseMovedEvent moved(input.X, input.Y); mCallback(moved);
                if (input.Primary != bPrimary)
                {
                    if (input.Primary) { MouseButtonPressedEvent event(MouseCode::ButtonLeft); mCallback(event); }
                    else { MouseButtonReleasedEvent event(MouseCode::ButtonLeft); mCallback(event); }
                }
            }
            bPrimary = input.Primary;
        }
        { std::lock_guard inputLock(state.Mutex); state.Input.Jump |= jump; state.Input.Pause |= pause; }

    }
    bool AndroidWindow::WaitForFrame()
    {
        auto& state = GetAndroidContext();
        for (;;)
        {
            PumpEvents();
            std::unique_lock lock(state.Mutex);
            if (state.Stop) return false;
            if (mWindow && state.Resumed && mWidth && mHeight)
            {
                lock.unlock();
                if (bAudioSuspended) AudioEngine::GetInstance().SetSuspended(false);
                bAudioSuspended = false;
                return true;
            }
            lock.unlock();
            if (!bAudioSuspended) AudioEngine::GetInstance().SetSuspended(true);
            bAudioSuspended = true;
            lock.lock();
            bResumePending = true;
            state.Changed.wait(lock, [&] {
                return state.Stop || state.WindowRevision != state.AppliedRevision || state.Resumed != bResumed;
            });
        }
    }
    bool AndroidWindow::ConsumeResume() { return std::exchange(bResumePending, false); }
    std::vector<const char*> AndroidWindow::GetVulkanInstanceExtensions() const
    {
        return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    }
    VkResult AndroidWindow::CreateVulkanSurface(VkInstance instance, PFN_vkGetInstanceProcAddr getProcAddr,
        VkSurfaceKHR* surface) const
    {
        if (!mWindow || !surface) return VK_ERROR_INITIALIZATION_FAILED;
        const auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(getProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
        if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        info.window = mWindow;
        return create(instance, &info, nullptr, surface);
    }
    void AndroidWindow::GetFramebufferExtent(uint32_t& width, uint32_t& height) const
    {
        width = mWindow ? static_cast<uint32_t>(std::max(ANativeWindow_getWidth(mWindow), 0)) : 0;
        height = mWindow ? static_cast<uint32_t>(std::max(ANativeWindow_getHeight(mWindow), 0)) : 0;
    }
}
