#include "Hepch.h"

#include "CommonWindow.h"
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
			int success = glfwInit();
			HE_CORE_ASSERT(success, "Could not intialize GLFW!");
			glfwSetErrorCallback(GLFWErrorCallback);
		}

		{
			// Rendering is owned by NVRHI swap chains; GLFW is input/windowing only.
			glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

			glfwWindowHint(GLFW_MAXIMIZED, props.bMaximized ? GLFW_TRUE : GLFW_FALSE);
			glfwWindowHint(GLFW_RESIZABLE, props.bResizable ? GLFW_TRUE : GLFW_FALSE);
			glfwWindowHint(GLFW_VISIBLE, props.bVisible ? GLFW_TRUE : GLFW_FALSE);
			mWindow = glfwCreateWindow((int)props.Width, (int)props.Height, mData.Title.c_str(), nullptr, nullptr);
			++s_GLFWWindowCount;

			// The platform may adjust the requested client size (for example when maximized).
			int width, height;
			glfwGetWindowSize(mWindow, &width, &height);
			mData.Width = width;
			mData.Height = height;
		}
		glfwSetWindowUserPointer(mWindow, &mData);
		ApplyInputMethodState(mWindow, mData.InputMethodEnabled);

		SetVSync(props.bVSync);

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
				glfwSetWindowIcon(mWindow, 1, &image);
				stbi_image_free(image.pixels);
			}
		}

		// Set GLFW callbacks
		glfwSetWindowSizeCallback(mWindow, [](GLFWwindow* window, int width, int height)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			data.Width = width;
			data.Height = height;

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

	void CommonWindow::OnUpdate()
	{
		HE_PROFILE;

		glfwPollEvents();
	}

	void CommonWindow::SetVSync(bool enabled)
	{
		mData.VSync = enabled;
	}

	bool CommonWindow::IsVSync() const
	{
		return mData.VSync;
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

		if (enabled)
		{
			glfwGetWindowPos(mWindow, &mWindowedX, &mWindowedY);
			glfwGetWindowSize(mWindow, &mWindowedWidth, &mWindowedHeight);
			mWindowedMaximized = glfwGetWindowAttrib(mWindow, GLFW_MAXIMIZED) == GLFW_TRUE;

			GLFWmonitor* monitor = FindBestMonitorForWindow(mWindow);
			const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
			if (!monitor || !mode)
			{
				HE_CORE_WARN("Could not determine a monitor for fullscreen mode");
				return;
			}

			glfwSetWindowMonitor(
				mWindow,
				monitor,
				0,
				0,
				mode->width,
				mode->height,
				mode->refreshRate);
			return;
		}

		const int restoreWidth = std::max(mWindowedWidth, 1);
		const int restoreHeight = std::max(mWindowedHeight, 1);
		glfwSetWindowMonitor(
			mWindow,
			nullptr,
			mWindowedX,
			mWindowedY,
			restoreWidth,
			restoreHeight,
			GLFW_DONT_CARE);
		if (mWindowedMaximized)
			glfwMaximizeWindow(mWindow);
	}

	bool CommonWindow::IsFullscreen() const
	{
		return mWindow && glfwGetWindowMonitor(mWindow) != nullptr;
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
