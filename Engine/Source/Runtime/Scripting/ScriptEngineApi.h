#pragma once

#include <ScriptApi.h>

namespace HEngine
{
    // Numeric operations and copied structures come from Api.schema.json.
    // Request pointers are borrowed only for the duration of this ABI call.
    class ScriptEngineApi final
    {
    public:
        static void BindThread();
        static int Call(ScriptAbi::Request* request) noexcept;
    };
}
