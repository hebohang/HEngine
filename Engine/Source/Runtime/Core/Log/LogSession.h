#pragma once

#include "Runtime/Core/Base/Base.h"

namespace spdlog { class logger; namespace sinks { class sink; } }

namespace HEngine
{
    // Game registration is synchronized with the log worker. Editor callers must
    // register on the UI thread because its logger intentionally remains synchronous.
    HENGINE_API void AddLogSink(const Ref<spdlog::logger>& logger, const Ref<spdlog::sinks::sink>& sink);
    // Invoke only after producers and application cleanup have completed.
    // Editor retains synchronous UI sinks; Game flushes and joins its log worker.
    HENGINE_API void ShutdownLogSession() noexcept;

    struct LogSession
    {
        ~LogSession() { ShutdownLogSession(); }
    };
}
