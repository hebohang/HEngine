#pragma once

#include "Runtime/Core/Base.h"
#include "Runtime/Core/Log.h"
#include <filesystem>

#ifdef HE_ENABLE_ASSERTS

	// Alteratively we could use the same "default" message for both "WITH_MSG" and "NO_MSG" and
	// provide support for custom formatting by concatenating the formatting string instead of having the format inside the default message
	#define HE_INTERNAL_ASSERT_IMPL(type, check, msg, ...) { if(!(check)) { HE##type##ERROR(msg, __VA_ARGS__); HE_DEBUGBREAK(); } }
	#define HE_INTERNAL_ASSERT_WITH_MSG(type, check, ...) HE_INTERNAL_ASSERT_IMPL(type, check, "Assertion failed: {0}", __VA_ARGS__)
	#define HE_INTERNAL_ASSERT_NO_MSG(type, check) HE_INTERNAL_ASSERT_IMPL(type, check, "Assertion '{0}' failed at {1}:{2}", HE_STRINGIFY_MACRO(check), std::filesystem::path(__FILE__).filename().string(), __LINE__)

	#define HE_INTERNAL_ASSERT_GET_MACRO_NAME(arg1, arg2, macro, ...) macro
	#define HE_INTERNAL_ASSERT_GET_MACRO(...) HE_EXPAND_MACRO( HE_INTERNAL_ASSERT_GET_MACRO_NAME(__VA_ARGS__, HE_INTERNAL_ASSERT_WITH_MSG, HE_INTERNAL_ASSERT_NO_MSG) )

	// Currently accepts at least the condition and one additional parameter (the message) being optional
	#define HE_ASSERT(...) HE_EXPAND_MACRO( HE_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_, __VA_ARGS__) )
	#define HE_CORE_ASSERT(...) HE_EXPAND_MACRO( HE_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_CORE_, __VA_ARGS__) )
#else
	#define HE_ASSERT(...)
	#define HE_CORE_ASSERT(...)
#endif
