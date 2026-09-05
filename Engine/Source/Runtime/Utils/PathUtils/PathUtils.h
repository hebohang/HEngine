#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace HEngine
{
    namespace Utils
    {
        enum class PathValidationErrorCode
        {
            None,
            EmptyPath,
            EmbeddedNull,
            AbsolutePath,
            ParentTraversal,
            InvalidComponent,
            ReservedComponent,
            InvalidRoot,
            ResolutionFailed,
            OutsideRoot
        };

        struct PathValidationResult
        {
            PathValidationErrorCode Code = PathValidationErrorCode::None;
            std::string Message;

            [[nodiscard]] bool Succeeded() const noexcept
            {
                return Code == PathValidationErrorCode::None;
            }

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return Succeeded();
            }
        };

        // Windows reserves these names even when an extension is present.
        // Keep this header-only so standalone tools (for example HCooker) use
        // exactly the same archive-path contract without linking the runtime.
        [[nodiscard]] inline bool IsWindowsReservedPathComponent(
            std::string_view component) noexcept
        {
            const std::size_t dot = component.find('.');
            const std::string_view baseName = component.substr(0, dot);
            const auto bEqualsAsciiIgnoreCase = [](std::string_view lhs, std::string_view rhs)
            {
                if (lhs.size() != rhs.size())
                    return false;
                for (std::size_t index = 0; index < lhs.size(); ++index)
                {
                    char lhsCharacter = lhs[index];
                    if (lhsCharacter >= 'A' && lhsCharacter <= 'Z')
                        lhsCharacter = static_cast<char>(lhsCharacter - 'A' + 'a');
                    if (lhsCharacter != rhs[index])
                        return false;
                }
                return true;
            };

            if (bEqualsAsciiIgnoreCase(baseName, "con") ||
                bEqualsAsciiIgnoreCase(baseName, "prn") ||
                bEqualsAsciiIgnoreCase(baseName, "aux") ||
                bEqualsAsciiIgnoreCase(baseName, "nul") ||
                bEqualsAsciiIgnoreCase(baseName, "conin$") ||
                bEqualsAsciiIgnoreCase(baseName, "conout$"))
            {
                return true;
            }

            if (baseName.size() < 4)
                return false;
            const std::string_view prefix = baseName.substr(0, 3);
            if (!bEqualsAsciiIgnoreCase(prefix, "com") &&
                !bEqualsAsciiIgnoreCase(prefix, "lpt"))
            {
                return false;
            }

            const std::string_view suffix = baseName.substr(3);
            return (suffix.size() == 1 && suffix.front() >= '1' && suffix.front() <= '9') ||
                suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3";
        }

        // Strict, ConfigManager-independent project path APIs. These reject
        // rooted and unsafe paths instead of preserving the legacy lenient behavior.
        [[nodiscard]] PathValidationResult TryNormalizeProjectRelativePath(
            std::string_view input,
            std::string& outNormalized);
        [[nodiscard]] PathValidationResult TryResolveProjectRelativePath(
            const std::filesystem::path& projectRoot,
            std::string_view input,
            std::filesystem::path& outFullPath,
            std::string& outNormalized);
        [[nodiscard]] std::string MakeProjectPathCacheKey(std::string_view normalizedPath);

        // Engine strings use UTF-8 at library boundaries while filesystem paths
        // retain the platform-native representation (UTF-16 on Windows).
        [[nodiscard]] std::filesystem::path Utf8ToPath(std::string_view path);
        [[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);
        [[nodiscard]] std::filesystem::path PathWithSuffix(
            std::filesystem::path path,
            std::string_view utf8Suffix);

        std::string NormalizeAssetPath(const std::string& path);
        bool IsSameAssetPath(const std::string& lhs, const std::string& rhs);
    }
}
