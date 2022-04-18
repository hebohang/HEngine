#pragma once

#include "Runtime/Core/Window.h"
#include "Runtime/Renderer/GraphicsContext.h"

#include <GLFW/glfw3.h>

namespace HEngine
{

	class WindowsWindow : public Window
	{
	public:
		WindowsWindow(const WindowProps& props);
		virtual ~WindowsWindow();

		void OnUpdate() override;

		inline uint32_t GetWidth() const override { return mData.Width; }
		inline uint32_t GetHeight() const override { return mData.Height; }

		// Window attributes
		inline void SetEventCallback(const EventCallbackFn& callback) override { mData.EventCallback = callback; }
		void SetVSync(bool enabled) override;
		bool IsVSync() const override;

		inline virtual void* GetNativeWindow() const { return mWindow; }
	private:
		virtual void Init(const WindowProps& props);
		virtual void Shutdown();
	private:
		GLFWwindow* mWindow;
		Scope<GraphicsContext> mContext;

		struct WindowData
		{
			std::string Title;
			uint32_t Width, Height;
			bool VSync;

			EventCallbackFn EventCallback;
		};

		WindowData mData;
	};

}