#pragma once

#include <span>
#include <cstdint>
#include <string>
#include <filesystem>
#include <stop_token>

namespace HEngine
{
    // Diagnostic snapshot only. The default heap excludes other heaps and
    // direct/driver allocations; these counters are not an atomic accounting.
    struct PlatformProcessMemorySnapshot
    {
        bool bProcessAvailable = false;
        bool bDefaultHeapAvailable = false;
        std::uint32_t ProcessError = 0;
        std::uint32_t DefaultHeapError = 0;
        std::uint64_t PrivateBytes = 0;
        std::uint64_t WorkingSetBytes = 0;
        std::uint64_t DefaultHeapAllocatedBytes = 0;
        std::uint64_t DefaultHeapCommittedBytes = 0;
        std::uint64_t DefaultHeapReservedBytes = 0;
    };

    struct PlatformProcessLaunchResult
    {
        bool bSuccess = false;
        std::string Error;
    };

    class PlatformProcess final
    {
    public:
        [[nodiscard]] static PlatformProcessMemorySnapshot ReadCurrentMemory() noexcept;
        struct CapturedResult
        {
            int ExitCode = -1;
            bool Cancelled = false;
            std::string Output;
        };
        // Worker-thread call. Owns the process tree, bounds output to 1 MiB,
        // and reaps children on cancellation, timeout and normal completion.
        static CapturedResult RunCaptured(const std::filesystem::path& executable,
            std::span<const std::string> arguments, std::stop_token stop,
            unsigned timeoutSeconds = 300);
        // Starts a new instance of the current executable. Call only after the
        // current Application has released its window and graphics device.
        [[nodiscard]] static PlatformProcessLaunchResult RelaunchCurrentProcess(
            std::span<const std::string> arguments) noexcept;
    };
}
