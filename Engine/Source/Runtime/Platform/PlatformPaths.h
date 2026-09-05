#pragma once

#include <filesystem>
#include <string_view>

namespace HEngine
{
    class PlatformPaths final
    {
    public:
        [[nodiscard]] static std::filesystem::path GetExecutableDirectory();
        [[nodiscard]] static std::filesystem::path GetLocalAppDataDirectory();

        static void ShowFatalDialog(std::string_view title, std::string_view message) noexcept;
    };
}
