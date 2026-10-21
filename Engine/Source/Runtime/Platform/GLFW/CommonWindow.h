#pragma once

#include "Runtime/Core/Window/Window.h"
#include "Runtime/Graphics/RHI/RenderSurface.h"
#if HE_RENDER_VULKAN
#include "Runtime/Platform/Vulkan/VulkanSurface.h"
#endif

#include <GLFW/glfw3.h>

namespace HEngine
{
	class CommonWindow final : public Window, public RenderSurface
#if HE_RENDER_VULKAN
        , public VulkanSurface
#endif
	{
		friend class Application;
	public:
		CommonWindow(const WindowProps& props);
		virtual ~CommonWindow();

		void OnUpdate() override;
		virtual void Shutdown() override;
		virtual void SetWindowTitle(const char* newTitle) override;

		uint32_t GetWidth() const override { return mData.Width; }
		uint32_t GetHeight() const override { return mData.Height; }
		void SetClientSize(uint32_t width, uint32_t height) override;

		// Window attributes
		void SetEventCallback(const EventCallbackFn& callback) override { mData.EventCallback = callback; }
		void SetVSync(bool enabled) override;
		bool IsVSync() const override;
		void SetFullscreen(bool enabled) override;
		bool IsFullscreen() const override;
		void SetInputMethodEnabled(bool enabled) override;
		void SetMouseCursorCaptured(bool captured) override;
		[[nodiscard]] bool IsMouseCursorCaptured() const override;
		void CenterMouseCursor() override;
		void SetMouseCursorPosition(float x, float y) override;
		void GetWindowScreenPosition(int32_t& x, int32_t& y) const override;
		bool SetMouseCursorImage(const std::filesystem::path& imagePath, uint32_t hotspotX, uint32_t hotspotY) override;
		void ResetMouseCursor() override;

		[[nodiscard]] GLFWwindow* GetGlfwWindow() const { return mWindow; }
#if HE_RENDER_VULKAN
        [[nodiscard]] std::vector<const char*> GetVulkanInstanceExtensions() const override;
        [[nodiscard]] VkResult CreateVulkanSurface(VkInstance instance,
            PFN_vkGetInstanceProcAddr getProcAddr, VkSurfaceKHR* surface) const override;
        void GetFramebufferExtent(uint32_t& width, uint32_t& height) const override;
        [[nodiscard]] bool GetSurfaceVSync() const override { return IsVSync(); }
#endif
	private:
		void Init(const WindowProps& props);
		void DispatchDeferredResizeEvent();
        void LogDisplayState(const char* reason) const;
	private:
		GLFWwindow* mWindow;
		GLFWcursor* mCustomCursor = nullptr;
		std::filesystem::path mCustomCursorPath;
		uint32_t mCustomCursorHotspotX = 0;
		uint32_t mCustomCursorHotspotY = 0;
		int mWindowedX = 0;
		int mWindowedY = 0;
		int mWindowedWidth = 1600;
		int mWindowedHeight = 900;
		bool mWindowedMaximized = false;
        bool mWindowedResizable = true;
        bool mBorderlessFullscreen = false;
        bool mFullscreenLocked = false;
		bool bAllowFullscreen = true;

		struct WindowData
		{
			std::string Title;
			uint32_t Width, Height;
			bool VSync;
			bool InputMethodEnabled = true;
			bool bDeferResizeEvent = false;
			bool bResizeEventDeferred = false;

			EventCallbackFn EventCallback;
		};

		WindowData mData;
	};

}
