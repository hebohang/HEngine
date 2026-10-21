#pragma once

#include <filesystem>
#include <string_view>

namespace HEngine
{
    class PlatformPaths final
    {
    public:
        [[nodiscard]] static std::filesystem::path GetExecutableDirectory();
        // Bundle Resources on macOS; executable-relative loose content elsewhere.
#if defined(__APPLE__)
        [[nodiscard]] static std::filesystem::path GetApplicationContentDirectory();
#else
        [[nodiscard]] static std::filesystem::path GetApplicationContentDirectory()
        {
            return GetExecutableDirectory();
        }
#endif
        [[nodiscard]] static std::filesystem::path GetLocalAppDataDirectory();
        // Editor tooling; resolve against this machine, never the build host.
        [[nodiscard]] static std::filesystem::path GetPowerShellExecutable();

        // Absolute native file-I/O spelling; never persist this in project data.
        [[nodiscard]] static std::filesystem::path ToFileIoPath(const std::filesystem::path& path);

        static void ShowFatalDialog(std::string_view title, std::string_view message) noexcept;
    };
}
