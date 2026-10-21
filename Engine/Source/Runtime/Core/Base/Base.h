#pragma once

#include <memory>

// Platform detection using predefined macros
#if defined(__EMSCRIPTEN__)
	#define HE_PLATFORM_WECHAT
#elif defined(_WIN32)
	/* Windows x64/x86 */
	#ifdef _WIN64
		/* Windows x64  */
		#define HE_PLATFORM_WINDOWS
	#else
		/* Windows x86 */
		#error "x86 Builds are not supported!"
	#endif
#elif defined(__APPLE__) || defined(__MACH__)
	#include <TargetConditionals.h>	
	/* TARGET_OS_MAC exists on all the platforms
	 * so we must check all of them (in this order)
	 * to ensure that we're running on MAC
	 * and not some other Apple platform */
	#if TARGET_IPHONE_SIMULATOR == 1
		#error "IOS simulator is not supported!"
	#elif TARGET_OS_IPHONE == 1
		#define HE_PLATFORM_IOS
		#error "IOS is not supported!"
	#elif TARGET_OS_MAC == 1
		#define HE_PLATFORM_MACOS
	#else
		#error "Unknown Apple platform!"
	#endif
 /* We also have to check __ANDROID__ before __linux__
  * since android is based on the linux kernel
  * it has __linux__ defined */
#elif defined(__ANDROID__)
	#define HE_PLATFORM_ANDROID
#elif defined(__linux__)
	#define HE_PLATFORM_LINUX
	#error "Linux is not supported!"
#else
	/* Unknown compiler/platform */
	#error "Unknown platform!"
#endif // End of platform detection


// DLL support
#ifdef HE_PLATFORM_WINDOWS
	#if HE_DYNAMIC_LINK
		#ifdef HE_BUILD_DLL
			#define HENGINE_API __declspec(dllexport)
		#else
			#define HENGINE_API __declspec(dllimport)
		#endif
	#else
		#define HENGINE_API
	#endif
#elif defined(HE_PLATFORM_WECHAT) || defined(HE_PLATFORM_ANDROID) || defined(HE_PLATFORM_MACOS)
	#define HENGINE_API
#else
	#error Unsupported HEngine platform
#endif // End of DLL support

#ifdef HE_DEBUG
	#if defined(HE_PLATFORM_WINDOWS)
		#define HE_DEBUGBREAK() __debugbreak()
	#elif defined(HE_PLATFORM_LINUX) || defined(HE_PLATFORM_ANDROID) || defined(HE_PLATFORM_MACOS)
		#include <signal.h>
		#define HE_DEBUGBREAK() raise(SIGTRAP)
	#else
		#error "Platform doesn't support debugbreak yet!"
	#endif
	#define HE_ENABLE_ASSERTS
#else 
	#define HE_DEBUGBREAK()
#endif

#ifndef WITH_EDITOR
	#define HE_FINAL 1
#else
	#define HE_FINAL 0
#endif

#define HE_EXPAND_MACRO(x) x
// https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html
#define HE_XSTRINGIFY_MACRO(x) HE_STRINGIFY_MACRO(x)
#define HE_STRINGIFY_MACRO(x) #x

#define HE_CONC(A, B) HE_CONC_(A, B)
#define HE_CONC_(A, B) A##B

#define BIT(x) (1 << x)

#define HE_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

namespace HEngine
{
	template<typename T>
	using Scope = std::unique_ptr<T>;
	template<typename T, typename ... Args>
	constexpr Scope<T> CreateScope(Args&& ... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	template<typename T>
	using Ref = std::shared_ptr<T>;
	template<typename T, typename ... Args>
	constexpr Ref<T> CreateRef(Args&& ... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}
	template<typename ToType, typename InType>
	Ref<ToType> RefCast(Ref<InType> arg)
	{
		auto p = std::dynamic_pointer_cast<ToType>(arg);
		if (p)
		{
			return p;
		}
		else
		{
			return nullptr;
		}
	}
}

#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Base/Assert.h"
