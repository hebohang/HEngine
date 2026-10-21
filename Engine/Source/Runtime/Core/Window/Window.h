#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Events/Event.h"

#include <filesystem>

namespace HEngine
{
	struct WindowProps
	{
		std::string Title;
		uint32_t Width;
		uint32_t Height;
		bool bMaximized;
		bool bResizable;
		bool bAllowFullscreen;
		bool bEnableInputMethod;
		bool bVisible;
		bool bVSync;
		// Automation requires the requested client extent, including when it
		// exceeds the desktop work area. Ordinary windows retain platform sizing.
		bool bExactClientSize = false;
        // Desktop-sized borderless window; never switches the monitor mode.
        bool bFullscreen = false;
        bool bLockFullscreen = false; // Explicit CLI mode overrides later preference application.

		WindowProps(const std::string& title = "HEngine",
			uint32_t width = 1600,
			uint32_t height = 900,
			bool maximized = true,
			bool resizable = true,
			bool allowFullscreen = true,
			bool enableInputMethod = true,
			bool visible = true,
			bool vsync = false)
			: Title(title), Width(width), Height(height), bMaximized(maximized), bResizable(resizable),
			bAllowFullscreen(allowFullscreen), bEnableInputMethod(enableInputMethod), bVisible(visible),
			bVSync(vsync)
		{
		}
	};

	// Interface representing a desktop system based Window
	class HENGINE_API Window
	{
	public:
		using EventCallbackFn = std::function<void(Event&)>;

		virtual ~Window() {}

		virtual void OnUpdate() = 0;
        // Mobile hosts may block without advancing the simulation while paused
        // or without a drawable surface. False requests an orderly shutdown.
        [[nodiscard]] virtual bool WaitForFrame() { return true; }
        [[nodiscard]] virtual bool ConsumeResume() { return false; }

		virtual void Shutdown() {}

		virtual void SetWindowTitle(const char* newTitle) {}

		[[nodiscard]] virtual uint32_t GetWidth() const = 0;
		[[nodiscard]] virtual uint32_t GetHeight() const = 0;
		// Exact client extent; platform implementation reports failure instead of
		// accepting desktop work-area clipping. Render resize events may be deferred.
		virtual void SetClientSize(uint32_t width, uint32_t height) = 0;

		// Window attributes
		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		virtual void SetVSync(bool enabled) = 0;
		virtual bool IsVSync() const = 0;
		virtual void SetFullscreen(bool enabled) {}
		[[nodiscard]] virtual bool IsFullscreen() const { return false; }
		virtual void SetInputMethodEnabled(bool enabled) {}
		virtual void SetMouseCursorCaptured(bool captured) {}
		[[nodiscard]] virtual bool IsMouseCursorCaptured() const { return false; }
		virtual void CenterMouseCursor() {}
		virtual void SetMouseCursorPosition(float x, float y) {}
		virtual void GetWindowScreenPosition(int32_t& x, int32_t& y) const { x = 0; y = 0; }
		virtual bool SetMouseCursorImage(const std::filesystem::path& imagePath, uint32_t hotspotX, uint32_t hotspotY) { return false; }
		virtual void ResetMouseCursor() {}

		static Scope<Window> Create(const WindowProps& props = WindowProps());
	};

}
