#pragma once

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#define HE_ENABLE_PROFILE 1

#define HE_PROFILE ZoneNamed(TracyConcat(__he_tracy_zone_, TracyLine), true)
#define HE_PROFILE_FUNCTION HE_PROFILE
#define HE_PROFILE_SECTION(name) ZoneNamedN(TracyConcat(__he_tracy_zone_, TracyLine), name, true)
#define HE_PROFILE_SECTION_COLOR(name, color) ZoneNamedNC(TracyConcat(__he_tracy_zone_, TracyLine), name, color, true)
#define HE_PROFILE_TRANSIENT_SECTION(name) ZoneTransientN(TracyConcat(__he_tracy_zone_, TracyLine), name, true)
#define HE_PROFILE_FRAME(name) FrameMarkNamed(name)
#define HE_PROFILE_THREAD(name) tracy::SetThreadName(name)
#define HE_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define HE_PROFILE_MESSAGE(text) TracyMessageL(text)
#define HE_PROFILE_MESSAGE_COLOR(text, color) TracyMessageLC(text, color)
#define HE_PROFILE_APP_INFO(text, size) TracyAppInfo(text, size)
#define HE_PROFILE_TEXT(text, size) ZoneText(text, size)
#define HE_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define HE_PROFILE_FREE(ptr) TracyFree(ptr)
#else
#define HE_ENABLE_PROFILE 0
#define HE_PROFILE
#define HE_PROFILE_FUNCTION
#define HE_PROFILE_SECTION(name)
#define HE_PROFILE_SECTION_COLOR(name, color)
#define HE_PROFILE_TRANSIENT_SECTION(name)
#define HE_PROFILE_FRAME(x)
#define HE_PROFILE_THREAD(name)
#define HE_PROFILE_PLOT(name, value)
#define HE_PROFILE_MESSAGE(text)
#define HE_PROFILE_MESSAGE_COLOR(text, color)
#define HE_PROFILE_APP_INFO(text, size)
#define HE_PROFILE_TEXT(text, size)
#define HE_PROFILE_ALLOC(ptr, size)
#define HE_PROFILE_FREE(ptr)
#endif
