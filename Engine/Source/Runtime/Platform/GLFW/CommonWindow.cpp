#include "Hepch.h"

#include "CommonWindow.h"
#include "Runtime/Platform/GLFW/GlfwInputEvents.h"
#include "Runtime/Platform/GLFW/GlfwEventModifiers.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Events/ApplicationEvent.h"
#include "Runtime/Events/MouseEvent.h"
#include "Runtime/Events/KeyEvent.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <stb_image/stb_image.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <vector>

#ifdef HE_PLATFORM_WINDOWS
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#pragma comment(lib, "imm32.lib")
#endif

namespace HEngine 
{
#if HE_RENDER_VULKAN
    std::vector<const char*> CommonWindow::GetVulkanInstanceExtensions() const
    {
        uint32_t count = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&count);
        if (extensions == nullptr || count == 0)
            throw std::runtime_error("GLFW could not enumerate Vulkan surface extensions");
        return {extensions, extensions + count};
    }

    VkResult CommonWindow::CreateVulkanSurface(VkInstance instance,
        PFN_vkGetInstanceProcAddr, VkSurfaceKHR* surface) const
    {
        if (mWindow == nullptr)
            return VK_ERROR_INITIALIZATION_FAILED;
        return glfwCreateWindowSurface(instance, mWindow, nullptr, surface);
    }

    void CommonWindow::GetFramebufferExtent(uint32_t& width, uint32_t& height) const
    {
        int nativeWidth = 0, nativeHeight = 0;
        if (mWindow != nullptr)
            glfwGetFramebufferSize(mWindow, &nativeWidth, &nativeHeight);
        width = static_cast<uint32_t>(std::max(nativeWidth, 0));
        height = static_cast<uint32_t>(std::max(nativeHeight, 0));
    }
#endif
	static uint8_t s_GLFWWindowCount = 0;

	static unsigned char* LoadRgbaImage(
		const std::filesystem::path& path,
		int& width,
		int& height)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		std::streamoff fileSize = -1;
		if (stream)
			fileSize = static_cast<std::streamoff>(stream.tellg());
		if (fileSize <= 0 || fileSize > std::numeric_limits<int>::max())
			return nullptr;

		std::vector<stbi_uc> fileBytes(static_cast<std::size_t>(fileSize));
		stream.seekg(0, std::ios::beg);
		if (!stream.read(
				reinterpret_cast<char*>(fileBytes.data()),
				static_cast<std::streamsize>(fileSize)))
			return nullptr;

		return stbi_load_from_memory(
			fileBytes.data(),
			static_cast<int>(fileBytes.size()),
			&width,
			&height,
			nullptr,
			4);
	}

	static void GLFWErrorCallback(int error, const char* description)
	{
		HE_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
	}

	static void ApplyInputMethodState(GLFWwindow* window, bool enabled)
	{
#ifdef HE_PLATFORM_WINDOWS
		if (!window)
			return;

		HWND hwnd = glfwGetWin32Window(window);
		if (!hwnd)
			return;

		HIMC inputContext = ::ImmGetContext(hwnd);
		if (!enabled)
		{
			if (!inputContext)
				return;

			::ImmNotifyIME(inputContext, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
			::ImmReleaseContext(hwnd, inputContext);
			::ImmAssociateContextEx(hwnd, nullptr, 0);
			return;
		}

		if (inputContext)
		{
			::ImmReleaseContext(hwnd, inputContext);
			return;
		}
		::ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
#else
		(void)window;
		(void)enabled;
#endif
	}

	static GLFWmonitor* FindBestMonitorForWindow(GLFWwindow* window)
	{
		if (!window)
			return glfwGetPrimaryMonitor();

		int windowX = 0;
		int windowY = 0;
		int windowWidth = 0;
		int windowHeight = 0;
		glfwGetWindowPos(window, &windowX, &windowY);
		glfwGetWindowSize(window, &windowWidth, &windowHeight);

		int monitorCount = 0;
		GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
		GLFWmonitor* bestMonitor = glfwGetPrimaryMonitor();
		int bestOverlap = 0;
		for (int index = 0; index < monitorCount; ++index)
		{
			int monitorX = 0;
			int monitorY = 0;
			glfwGetMonitorPos(monitors[index], &monitorX, &monitorY);
			const GLFWvidmode* mode = glfwGetVideoMode(monitors[index]);
			if (!mode)
				continue;

			const int overlapWidth = std::max(0,
				std::min(windowX + windowWidth, monitorX + mode->width) - std::max(windowX, monitorX));
			const int overlapHeight = std::max(0,
				std::min(windowY + windowHeight, monitorY + mode->height) - std::max(windowY, monitorY));
			const int overlap = overlapWidth * overlapHeight;
			if (overlap > bestOverlap)
			{
				bestOverlap = overlap;
				bestMonitor = monitors[index];
			}
		}
		return bestMonitor;
	}

	Scope<Window> Window::Create(const WindowProps& props)
	{
		return CreateScope<CommonWindow>(props);
	}

	CommonWindow::CommonWindow(const WindowProps& props)
	{
		HE_PROFILE;

		Init(props);
	}

	CommonWindow::~CommonWindow()
	{
		Shutdown();
	}

	void CommonWindow::Init(const WindowProps& props)
	{
		HE_PROFILE;

		mData.Title = props.Title;
		mData.Width = props.Width;
		mData.Height = props.Height;
		mData.InputMethodEnabled = props.bEnableInputMethod;
		bAllowFullscreen = props.bAllowFullscreen;
        mFullscreenLocked = props.bLockFullscreen;
        mWindowedWidth = static_cast<int>(props.Width);
        mWindowedHeight = static_cast<int>(props.Height);
        mWindowedMaximized = props.bMaximized;
        mWindowedResizable = props.bResizable;

		HE_CORE_INFO("Creating window {0} ({1}, {2})", props.Title, props.Width, props.Height);

#ifdef HE_PLATFORM_WINDOWS
		// ImmDisableIME must run before the first top-level window is created. This
		// also covers modern TSF-based IMEs that are not fully disabled by merely
		// dissociating an IMM32 context after window creation.
		if (!props.bEnableInputMethod && !::ImmDisableIME(::GetCurrentThreadId()))
			HE_CORE_WARN("Could not disable the input method before creating the game window");
#endif

		if (s_GLFWWindowCount == 0)
		{
			HE_PROFILE_SECTION("glfwInit");
			glfwSetErrorCallback(GLFWErrorCallback);
#if defined(__APPLE__) && HE_RENDER_VULKAN
			// Use the same app-local MoltenVK as the rendering and interop code.
			// This also lets standalone .app bundles run without a Vulkan SDK/ICD.
			glfwInitVulkanLoader(&vkGetInstanceProcAddr);
#endif
			int success = glfwInit();
			HE_CORE_ASSERT(success, "Could not intialize GLFW!");
		}

		{
			// Rendering is owned by NVRHI swap chains; GLFW is input/windowing only.
			glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            mBorderlessFullscreen = props.bFullscreen && props.bAllowFullscreen && !props.bExactClientSize;
            int initialWidth = static_cast<int>(props.Width), initialHeight = static_cast<int>(props.Height);
            if (mBorderlessFullscreen)
            {
                GLFWmonitor* monitor = glfwGetPrimaryMonitor();
                const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
                if (!mode)
                    throw std::runtime_error("Cannot create a borderless fullscreen window without a desktop monitor.");
                int x = 0, y = 0;
                glfwGetMonitorPos(monitor, &x, &y);
                glfwWindowHint(GLFW_POSITION_X, x);
                glfwWindowHint(GLFW_POSITION_Y, y);
                initialWidth = mode->width;
                initialHeight = mode->height;
                // Restore to the requested ordinary window, centered on this monitor.
                mWindowedX = x + std::max(0, (initialWidth - mWindowedWidth) / 2);
                mWindowedY = y + std::max(0, (initialHeight - mWindowedHeight) / 2);
            }
            else
            {
                glfwWindowHint(GLFW_POSITION_X, GLFW_ANY_POSITION);
                glfwWindowHint(GLFW_POSITION_Y, GLFW_ANY_POSITION);
            }
            // Desktop mode is already in framebuffer pixels on Win32. Never
            // multiply it by the display content scale a second time.
            glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
            glfwWindowHint(GLFW_DECORATED, mBorderlessFullscreen ? GLFW_FALSE : GLFW_TRUE);
			glfwWindowHint(GLFW_MAXIMIZED, props.bMaximized && !mBorderlessFullscreen ? GLFW_TRUE : GLFW_FALSE);
			glfwWindowHint(GLFW_RESIZABLE, props.bResizable && !mBorderlessFullscreen ? GLFW_TRUE : GLFW_FALSE);
			glfwWindowHint(GLFW_VISIBLE, props.bVisible ? GLFW_TRUE : GLFW_FALSE);
            // A null monitor keeps this a desktop window, avoiding exclusive
            // display mode switches, black flashes and refresh-rate changes.
			mWindow = glfwCreateWindow(initialWidth, initialHeight, mData.Title.c_str(), nullptr, nullptr);
			if (!mWindow)
				throw std::runtime_error("GLFW failed to create the requested window");
			++s_GLFWWindowCount;
			if (props.bExactClientSize)
			{
				// Win32 can constrain CreateWindow to the monitor work area. Explicit
				// tracking limits and a post-creation resize preserve native benchmark
				// dimensions without fullscreen or changing the monitor display mode.
				glfwSetWindowSizeLimits(mWindow, static_cast<int>(props.Width),
					static_cast<int>(props.Height), static_cast<int>(props.Width),
					static_cast<int>(props.Height));
				glfwSetWindowSize(mWindow, static_cast<int>(props.Width),
					static_cast<int>(props.Height));
			}

			// The platform may adjust the requested client size (for example when maximized).
			int width, height;
			glfwGetWindowSize(mWindow, &width, &height);
			if (props.bExactClientSize &&
				(width != static_cast<int>(props.Width) || height != static_cast<int>(props.Height)))
			{
				glfwDestroyWindow(mWindow);
				mWindow = nullptr;
				--s_GLFWWindowCount;
				if (s_GLFWWindowCount == 0)
					glfwTerminate();
				throw std::runtime_error("The platform could not preserve the requested exact client resolution");
			}
			mData.Width = width;
			mData.Height = height;
		}
		glfwSetWindowUserPointer(mWindow, &mData);
		ApplyInputMethodState(mWindow, mData.InputMethodEnabled);

		SetVSync(props.bVSync);
        LogDisplayState("created");

		std::filesystem::path iconPath;
#if WITH_EDITOR
		iconPath = ConfigManager::GetEngineFullPath("Resources/Icons/EngineIcon.png");
#else
		const auto gameConfigPath = ConfigManager::GetProjectFullPath(
			ConfigManager::GetInstance().GetGameConfigPath().generic_string());
		if (std::filesystem::exists(gameConfigPath))
		{
			const Json gameConfig = Utils::LoadJsonFile(gameConfigPath);
			if (gameConfig.contains("Game") && gameConfig["Game"].contains("Icon"))
				iconPath = ConfigManager::GetFullPath(gameConfig["Game"]["Icon"].get<std::string>());
		}
#endif
		if (!iconPath.empty())
		{
			GLFWimage image{};
			image.pixels = LoadRgbaImage(iconPath, image.width, image.height);
			if (image.pixels)
			{
#if !defined(__APPLE__)
				glfwSetWindowIcon(mWindow, 1, &image);
#endif
				stbi_image_free(image.pixels);
			}
		}

		// Set GLFW callbacks
		glfwSetWindowSizeCallback(mWindow, [](GLFWwindow* window, int width, int height)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			data.Width = width;
			data.Height = height;
			if (data.bDeferResizeEvent)
			{
				data.bResizeEventDeferred = true;
				return;
			}

			WindowResizeEvent event(width, height);
			data.EventCallback(event);
		});

		glfwSetWindowCloseCallback(mWindow, [](GLFWwindow* window)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			WindowCloseEvent event;
			data.EventCallback(event);
		});

		glfwSetWindowFocusCallback(mWindow, [](GLFWwindow* window, int focused)
		{
			if (focused != GLFW_TRUE)
				return;

			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			if (!data.InputMethodEnabled)
				ApplyInputMethodState(window, false);
		});

		glfwSetKeyCallback(mWindow, [](GLFWwindow* window, int key, int scancode, int action, int mods)
		{
#if defined(HE_DEV_AGENT_RUNTIME)
			if (Input::IsSyntheticInputSessionActive())
				return;
#endif
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
            const ScopedGlfwEventModifiers eventModifiers(mods);

			switch (action)
			{
				case GLFW_PRESS:
				{
					KeyPressedEvent event(static_cast<KeyCode>(key), 0);
					data.EventCallback(event);
					break;
				}
				case GLFW_RELEASE:
				{
					KeyReleasedEvent event(static_cast<KeyCode>(key));
					data.EventCallback(event);
					break;
				}
				case GLFW_REPEAT:
				{
					KeyPressedEvent event(static_cast<KeyCode>(key), 1);
					data.EventCallback(event);
					break;
				}
			}
		});

		glfwSetCharCallback(mWindow, [](GLFWwindow* window, unsigned int keycode)
		{
#if defined(HE_DEV_AGENT_RUNTIME)
			if (Input::IsSyntheticInputSessionActive())
				return;
#endif
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			if (!data.InputMethodEnabled)
				return;

			KeyTypedEvent event(static_cast<KeyCode>(keycode));
			data.EventCallback(event);
		});

		glfwSetMouseButtonCallback(mWindow, [](GLFWwindow* window, int button, int action, int mods)
		{
#if defined(HE_DEV_AGENT_RUNTIME)
			if (Input::IsSyntheticInputSessionActive())
				return;
#endif
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
            const ScopedGlfwEventModifiers eventModifiers(mods);
            const ScopedGlfwMouseEventPosition eventPosition(window);

			switch (action)
			{
				case GLFW_PRESS:
				{
					MouseButtonPressedEvent event(static_cast<MouseCode>(button));
					data.EventCallback(event);
					break;
				}
				case GLFW_RELEASE:
				{
					MouseButtonReleasedEvent event(static_cast<MouseCode>(button));
					data.EventCallback(event);
					break;
				}
			}
		});

		glfwSetScrollCallback(mWindow, [](GLFWwindow* window, double xOffset, double yOffset)
		{
#if defined(HE_DEV_AGENT_RUNTIME)
			if (Input::IsSyntheticInputSessionActive())
				return;
#endif
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			MouseScrolledEvent event((float)xOffset, (float)yOffset);
			data.EventCallback(event);
		});

		glfwSetCursorPosCallback(mWindow, [](GLFWwindow* window, double xPos, double yPos)
		{
#if defined(HE_DEV_AGENT_RUNTIME)
			if (Input::IsSyntheticInputSessionActive())
				return;
#endif
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			MouseMovedEvent event((float)xPos, (float)yPos);
			data.EventCallback(event);
		});
	}

	void CommonWindow::Shutdown()
	{
		HE_PROFILE;

		if (mWindow == nullptr)
			return;

		glfwSetWindowUserPointer(mWindow, nullptr);
		glfwSetWindowSizeCallback(mWindow, nullptr);
		glfwSetWindowCloseCallback(mWindow, nullptr);
		glfwSetWindowFocusCallback(mWindow, nullptr);
		glfwSetKeyCallback(mWindow, nullptr);
		glfwSetCharCallback(mWindow, nullptr);
		glfwSetMouseButtonCallback(mWindow, nullptr);
		glfwSetScrollCallback(mWindow, nullptr);
		glfwSetCursorPosCallback(mWindow, nullptr);
		if (mCustomCursor)
		{
			glfwDestroyCursor(mCustomCursor);
			mCustomCursor = nullptr;
			mCustomCursorPath.clear();
			mCustomCursorHotspotX = 0;
			mCustomCursorHotspotY = 0;
		}

#ifdef HE_PLATFORM_WINDOWS
		{
			HWND hwnd = glfwGetWin32Window(mWindow);
			if (hwnd)
				::ImmAssociateContextEx(hwnd, NULL, 0);
		}
#endif

		glfwDestroyWindow(mWindow);
		mWindow = nullptr;

		if (s_GLFWWindowCount > 0)
			--s_GLFWWindowCount;

		if (s_GLFWWindowCount == 0)
		{
		#ifdef HE_PLATFORM_WINDOWS
			// Workaround: in some environments (IME/TSF active), glfwTerminate may crash in
			// _glfwTerminateWin32 during process shutdown (msctf.dll/imm32.dll stack).
			// We skip it on Windows because the process exits immediately after Application::Clean().
			// This keeps teardown stable without modifying third-party GLFW source.
			return;
		#endif
			glfwTerminate();
		}
	}

	void CommonWindow::SetWindowTitle(const char* newTitle)
	{
		glfwSetWindowTitle(mWindow, newTitle);
	}

	void CommonWindow::SetClientSize(uint32_t width, uint32_t height)
	{
		if (!mWindow || width < 64 || height < 64 || width > 8192 || height > 8192)
			throw std::invalid_argument("Exact window client dimensions must be in 64..8192");
		if (IsFullscreen())
			throw std::logic_error("Exact client resize requires a windowed surface");
		// Do not resize GPU resources inside a possible GLFW/UI callback. The
		// existing OnUpdate frame-boundary dispatch owns renderer resize/safety.
		mData.bDeferResizeEvent = true;
		glfwRestoreWindow(mWindow);
		glfwSetWindowSizeLimits(mWindow, static_cast<int>(width), static_cast<int>(height),
			static_cast<int>(width), static_cast<int>(height));
		glfwSetWindowSize(mWindow, static_cast<int>(width), static_cast<int>(height));
		int actualWidth = 0, actualHeight = 0;
		glfwGetWindowSize(mWindow, &actualWidth, &actualHeight);
		if (actualWidth != static_cast<int>(width) || actualHeight != static_cast<int>(height))
			throw std::runtime_error("The platform could not preserve the requested exact client extent");
		mData.Width = width;
		mData.Height = height;
		mData.bResizeEventDeferred = true;
		mWindowedWidth = actualWidth;
		mWindowedHeight = actualHeight;
		mWindowedMaximized = false;
	}

	void CommonWindow::OnUpdate()
	{
		HE_PROFILE;

		glfwPollEvents();
		// Fullscreen changes may be requested while the current UI frame is being
		// built. Resize the renderer only after that frame has been submitted.
		DispatchDeferredResizeEvent();
	}

	void CommonWindow::SetVSync(bool enabled)
	{
		mData.VSync = enabled;
	}

	bool CommonWindow::IsVSync() const
	{
		return mData.VSync;
	}

	void CommonWindow::DispatchDeferredResizeEvent()
	{
		mData.bDeferResizeEvent = false;
		if (!mData.bResizeEventDeferred)
			return;

		mData.bResizeEventDeferred = false;
		WindowResizeEvent event(mData.Width, mData.Height);
		mData.EventCallback(event);
	}

	void CommonWindow::SetFullscreen(bool enabled)
	{
		if (!bAllowFullscreen)
		{
			if (enabled)
				HE_CORE_INFO("Ignoring fullscreen request while fixed window sizing is active");
			return;
		}
		if (!mWindow || enabled == IsFullscreen())
			return;
        if (mFullscreenLocked)
        {
            HE_CORE_INFO("Ignoring fullscreen preference because the command line fixes the window mode");
            return;
        }

		if (enabled)
		{
			GLFWmonitor* monitor = FindBestMonitorForWindow(mWindow);
			const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
			if (!monitor || !mode)
			{
				HE_CORE_WARN("Could not determine a monitor for fullscreen mode");
				return;
			}

			mData.bDeferResizeEvent = true;
            mWindowedMaximized = glfwGetWindowAttrib(mWindow, GLFW_MAXIMIZED) == GLFW_TRUE;
            mWindowedResizable = glfwGetWindowAttrib(mWindow, GLFW_RESIZABLE) == GLFW_TRUE;
            if (mWindowedMaximized)
                glfwRestoreWindow(mWindow);
            glfwGetWindowPos(mWindow, &mWindowedX, &mWindowedY);
            glfwGetWindowSize(mWindow, &mWindowedWidth, &mWindowedHeight);
            int x = 0, y = 0;
            glfwGetMonitorPos(monitor, &x, &y);
            glfwSetWindowAttrib(mWindow, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowAttrib(mWindow, GLFW_RESIZABLE, GLFW_FALSE);
            glfwSetWindowPos(mWindow, x, y);
            glfwSetWindowSize(mWindow, mode->width, mode->height);
            mBorderlessFullscreen = true;
		}
        else
        {
            mData.bDeferResizeEvent = true;
            glfwSetWindowAttrib(mWindow, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowAttrib(mWindow, GLFW_RESIZABLE, mWindowedResizable ? GLFW_TRUE : GLFW_FALSE);
            glfwSetWindowSize(mWindow, std::max(mWindowedWidth, 1), std::max(mWindowedHeight, 1));
            glfwSetWindowPos(mWindow, mWindowedX, mWindowedY);
            if (mWindowedMaximized)
                glfwMaximizeWindow(mWindow);
            mBorderlessFullscreen = false;
        }
        int width = 0, height = 0;
        glfwGetWindowSize(mWindow, &width, &height);
        mData.Width = static_cast<uint32_t>(std::max(width, 0));
        mData.Height = static_cast<uint32_t>(std::max(height, 0));
        mData.bResizeEventDeferred = true;
        LogDisplayState("mode changed");
	}

	bool CommonWindow::IsFullscreen() const
	{
		return mWindow && mBorderlessFullscreen;
	}

    void CommonWindow::LogDisplayState(const char* reason) const
    {
        if (!mWindow) return;
        GLFWmonitor* monitor = FindBestMonitorForWindow(mWindow);
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        int framebufferWidth = 0, framebufferHeight = 0, x = 0, y = 0;
        float scaleX = 1, scaleY = 1;
        glfwGetFramebufferSize(mWindow, &framebufferWidth, &framebufferHeight);
        glfwGetWindowPos(mWindow, &x, &y);
        glfwGetWindowContentScale(mWindow, &scaleX, &scaleY);
        HE_CORE_INFO("[Display] {} mode={} client={}x{} framebuffer={}x{} position={},{} contentScale={},{} monitor={} desktop={}x{} refresh={}Hz exclusive={}",
            reason, IsFullscreen() ? "BorderlessFullscreen" : "Windowed", mData.Width, mData.Height,
            framebufferWidth, framebufferHeight, x, y, scaleX, scaleY,
            monitor ? glfwGetMonitorName(monitor) : "unknown", mode ? mode->width : 0,
            mode ? mode->height : 0, mode ? mode->refreshRate : 0,
            glfwGetWindowMonitor(mWindow) != nullptr);
    }

	bool CommonWindow::SetMouseCursorImage(const std::filesystem::path& imagePath, uint32_t hotspotX, uint32_t hotspotY)
	{
		if (!mWindow || imagePath.empty())
			return false;

		if (mCustomCursor &&
			mCustomCursorPath == imagePath &&
			mCustomCursorHotspotX == hotspotX &&
			mCustomCursorHotspotY == hotspotY)
		{
			glfwSetCursor(mWindow, mCustomCursor);
			return true;
		}

		int width = 0;
		int height = 0;
		unsigned char* pixels = LoadRgbaImage(imagePath, width, height);
		if (!pixels || width <= 0 || height <= 0)
		{
			if (pixels)
				stbi_image_free(pixels);
			HE_CORE_WARN("Could not load custom mouse cursor: {0}", Utils::PathToUtf8(imagePath));
			return false;
		}

		GLFWimage image{ width, height, pixels };
		GLFWcursor* cursor = glfwCreateCursor(
			&image,
			static_cast<int>(std::min<uint32_t>(hotspotX, static_cast<uint32_t>(width - 1))),
			static_cast<int>(std::min<uint32_t>(hotspotY, static_cast<uint32_t>(height - 1))));
		stbi_image_free(pixels);
		if (!cursor)
		{
			HE_CORE_WARN("Could not create custom mouse cursor: {0}", Utils::PathToUtf8(imagePath));
			return false;
		}

		GLFWcursor* previousCursor = mCustomCursor;
		mCustomCursor = cursor;
		mCustomCursorPath = imagePath;
		mCustomCursorHotspotX = hotspotX;
		mCustomCursorHotspotY = hotspotY;
		glfwSetCursor(mWindow, mCustomCursor);
		if (previousCursor)
			glfwDestroyCursor(previousCursor);
		return true;
	}

	void CommonWindow::ResetMouseCursor()
	{
		if (mWindow)
			glfwSetCursor(mWindow, nullptr);
	}

	void CommonWindow::SetInputMethodEnabled(bool enabled)
	{
		mData.InputMethodEnabled = enabled;
		ApplyInputMethodState(mWindow, enabled);
	}

	void CommonWindow::SetMouseCursorCaptured(bool captured)
	{
		if (!mWindow)
			return;

		glfwSetInputMode(mWindow, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
		if (glfwRawMouseMotionSupported())
			glfwSetInputMode(mWindow, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
	}

	bool CommonWindow::IsMouseCursorCaptured() const
	{
		return mWindow && glfwGetInputMode(mWindow, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
	}

	void CommonWindow::CenterMouseCursor()
	{
		if (!mWindow)
			return;

		int width = 0;
		int height = 0;
		glfwGetWindowSize(mWindow, &width, &height);
		glfwSetCursorPos(mWindow, static_cast<double>(width) * 0.5, static_cast<double>(height) * 0.5);
	}

	void CommonWindow::SetMouseCursorPosition(float x, float y)
	{
		if (mWindow)
			glfwSetCursorPos(mWindow, static_cast<double>(x), static_cast<double>(y));
	}

	void CommonWindow::GetWindowScreenPosition(int32_t& x, int32_t& y) const
	{
		x = 0;
		y = 0;
		if (mWindow)
			glfwGetWindowPos(mWindow, &x, &y);
	}

}
