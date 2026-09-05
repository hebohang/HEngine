#pragma once

#include <span>
#include <string>

namespace HEngine
{
    struct PlatformProcessLaunchResult
    {
        bool bSuccess = false;
        std::string Error;
    };

    class PlatformProcess final
    {
    public:
        // Starts a new instance of the current executable. Call only after the
        // current Application has released its window and graphics device.
        [[nodiscard]] static PlatformProcessLaunchResult RelaunchCurrentProcess(
            std::span<const std::string> arguments) noexcept;
    };
}
