#pragma once

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
#else 
	#error HEngine only supports Windows!
#endif

#ifdef HZ_DEBUG
	#define HE_ENABLE_ASSERTS
#endif 

#ifdef HE_ENABLE_ASSERTS
	#define HE_ASSERT(x, ...) { if(!(x)) { HE_ERROR("Assertion Failed: {0}", __VA_ARGS__); __debugbreak(); } }
	#define HE_CORE_ASSERT(x, ...) { if(!(x)) { HE_CORE_ERROR("Assertion Failed: {0}", __VA_ARGS__); __debugbreak(); } }
#else	 
	#define HE_ASSERT(x, ...)
	#define HE_CORE_ASSERT(x, ...)
#endif

#define BIT(x) (1 << x)

#define HE_BIND_EVENT_FN(fn) std::bind(&fn, this, std::placeholders::_1)