#pragma once

#include <ScriptApi.h>

namespace HEngine
{
    using AndroidManagedEntry = int(*)(ScriptAbi::Request*);
    [[nodiscard]] AndroidManagedEntry GetAndroidManagedEntry();
}
