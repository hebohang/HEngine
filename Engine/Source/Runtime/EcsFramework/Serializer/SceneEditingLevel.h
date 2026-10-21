#pragma once
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Meta/Json.h"

namespace HEngine
{
    class Level;
    // Existing component add/remove lifecycle + generated serializers. Caller
    // owns the frame boundary and rollback; never pass a running simulation.
    void ApplySceneEditingValues(const Ref<Level>& level, const Json& values);
}
