#pragma once

#include <filesystem>
#include <span>

namespace HEngine
{
    struct ExternalApplicationStateFile
    {
        std::filesystem::path DefaultPath;
        std::filesystem::path RuntimePath;
        std::filesystem::path CachePath;
    };

    class FileDialogs
    {
    public:
        // These return empty strings if cancelled
        static std::filesystem::path OpenFile(const char* filter, std::string initialDir = "");
        static std::filesystem::path SaveFile(const char* filter, std::string initialDir = "");
        static std::filesystem::path SelectFolder(std::string initialDir = "");
        static void OpenInExplorer(std::string filePath);
        [[nodiscard]] static bool OpenFileWithApplication(const std::filesystem::path& applicationPath,
                                                          const std::filesystem::path& filePath);
        [[nodiscard]] static bool OpenFileWithApplicationAndStateCache(
            const std::filesystem::path& applicationPath,
            const std::filesystem::path& filePath,
            std::span<const ExternalApplicationStateFile> stateFiles);
    };
}
