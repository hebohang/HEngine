#pragma once

#include <cstdint>

namespace HEngine
{
    // Cumulative OS-scheduled kernel + user CPU time, in 100 ns units.
    // Storage precision does not imply scheduler/accounting resolution.
    struct CpuExecutionTimes
    {
        uint64_t ThreadId = 0;
        uint64_t Thread100Nanoseconds = 0;
        uint64_t Process100Nanoseconds = 0;
        bool bValid = false;
    };

    // Current calling thread and its process. Platform implementation only.
    [[nodiscard]] CpuExecutionTimes QueryCpuExecutionTimes() noexcept;
}
