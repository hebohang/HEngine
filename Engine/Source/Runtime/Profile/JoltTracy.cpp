#include "Hepch.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Profiler.h>

#if defined(TRACY_ENABLE) && defined(JPH_EXTERNAL_PROFILE)

namespace
{
    constexpr uint32_t JoltTracyStorageBytes = 64;
}

JPH_NAMESPACE_BEGIN

ExternalProfileMeasurement::ExternalProfileMeasurement(const char* inName, uint32 inColor)
{
    static_assert(sizeof(tracy::ScopedZone) <= JoltTracyStorageBytes, "Jolt Tracy storage is too small for tracy::ScopedZone");
    static_assert(alignof(ExternalProfileMeasurement) >= alignof(tracy::ScopedZone), "Jolt Tracy storage alignment is insufficient");

    const char* name = inName != nullptr ? inName : "Jolt";
    const size_t nameSize = inName != nullptr ? strlen(inName) : 4;

    new (mUserData) tracy::ScopedZone(
        __LINE__,
        __FILE__,
        strlen(__FILE__),
        JPH_FUNCTION_NAME,
        strlen(JPH_FUNCTION_NAME),
        name,
        nameSize,
        inColor,
        TRACY_CALLSTACK,
        true);
}

ExternalProfileMeasurement::~ExternalProfileMeasurement()
{
    reinterpret_cast<tracy::ScopedZone*>(mUserData)->~ScopedZone();
}

JPH_NAMESPACE_END

#endif
