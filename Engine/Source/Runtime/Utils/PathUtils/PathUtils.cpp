#include "Hepch.h"

#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Platform/PlatformPaths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace HEngine::Utils
{
    namespace
    {
        static std::string ToSlash(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            return value;
        }

        static PathValidationResult MakePathValidationError(PathValidationErrorCode code, std::string message)
        {
            PathValidationResult result;
            result.Code = code;
            result.Message = std::move(message);
            return result;
        }

        static bool HasWindowsDrivePrefix(std::string_view path)
        {
            return path.size() >= 2 &&
                std::isalpha(static_cast<unsigned char>(path[0])) != 0 &&
                path[1] == ':';
        }

        static bool IsRootedPath(std::string_view slashPath)
        {
            if (slashPath.empty())
                return false;
            if (slashPath.front() == '/' || HasWindowsDrivePrefix(slashPath))
                return true;

            const std::filesystem::path fsPath = Utf8ToPath(slashPath);
            return fsPath.is_absolute() || fsPath.has_root_name() || fsPath.has_root_directory();
        }

        static bool IsInvalidWindowsComponent(std::string_view component)
        {
            if (component.empty() || component.back() == '.' || component.back() == ' ')
                return true;
            if (component.find_first_of("<>:\"|?*") != std::string_view::npos)
                return true;

            return std::any_of(component.begin(), component.end(), [](char character)
            {
                return static_cast<unsigned char>(character) < 32;
            });
        }

        static bool IsWithinRoot(
            const std::filesystem::path& canonicalRoot,
            const std::filesystem::path& candidate)
        {
            const std::filesystem::path relative = candidate.lexically_relative(canonicalRoot);
            if (relative.empty())
                return candidate == canonicalRoot;
            if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
                return false;
            for (const std::filesystem::path& component : relative)
            {
                if (component == "..")
                    return false;
            }
            return true;
        }
    }

    PathValidationResult TryNormalizeProjectRelativePath(
        std::string_view input,
        std::string& outNormalized)
    {
        outNormalized.clear();
        if (input.empty())
            return MakePathValidationError(PathValidationErrorCode::EmptyPath, "Project path is empty.");
        if (input.find('\0') != std::string_view::npos)
            return MakePathValidationError(PathValidationErrorCode::EmbeddedNull, "Project path contains an embedded null.");

        const std::string slashPath = ToSlash(std::string(input));
        if (IsRootedPath(slashPath))
        {
            return MakePathValidationError(
                PathValidationErrorCode::AbsolutePath,
                "Project path must be relative and cannot contain a root name.");
        }

        std::string normalizedPath;
        std::size_t componentBegin = 0;
        while (componentBegin <= slashPath.size())
        {
            const std::size_t separator = slashPath.find('/', componentBegin);
            const std::size_t componentEnd = separator == std::string::npos ? slashPath.size() : separator;
            const std::string_view component(slashPath.data() + componentBegin, componentEnd - componentBegin);

            if (component == "..")
            {
                return MakePathValidationError(
                    PathValidationErrorCode::ParentTraversal,
                    "Project path cannot contain a parent traversal component.");
            }
            if (!component.empty() && component != ".")
            {
                if (IsInvalidWindowsComponent(component))
                {
                    return MakePathValidationError(
                        PathValidationErrorCode::InvalidComponent,
                        "Project path contains an invalid Windows path component: " + std::string(component));
                }
                if (IsWindowsReservedPathComponent(component))
                {
                    return MakePathValidationError(
                        PathValidationErrorCode::ReservedComponent,
                        "Project path contains a reserved Windows path component: " + std::string(component));
                }

                if (!normalizedPath.empty())
                    normalizedPath.push_back('/');
                normalizedPath.append(component);
            }

            if (separator == std::string::npos)
                break;
            componentBegin = separator + 1;
        }

        if (normalizedPath.empty())
            return MakePathValidationError(PathValidationErrorCode::EmptyPath, "Project path resolves to an empty value.");

        outNormalized = std::move(normalizedPath);
        return {};
    }

    PathValidationResult TryResolveProjectRelativePath(
        const std::filesystem::path& projectRoot,
        std::string_view input,
        std::filesystem::path& outFullPath,
        std::string& outNormalized)
    {
        outFullPath.clear();
        outNormalized.clear();

        std::string normalizedPath;
        const PathValidationResult validation = TryNormalizeProjectRelativePath(input, normalizedPath);
        if (!validation)
            return validation;
        if (projectRoot.empty())
            return MakePathValidationError(PathValidationErrorCode::InvalidRoot, "Project root is empty.");

        std::error_code error;
        const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, error);
        if (error)
        {
            return MakePathValidationError(
                PathValidationErrorCode::ResolutionFailed,
                "Cannot make the project root absolute: " + error.message());
        }

        const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
        if (error)
        {
            return MakePathValidationError(
                PathValidationErrorCode::ResolutionFailed,
                "Cannot resolve the project root: " + error.message());
        }

        if (!std::filesystem::is_directory(canonicalRoot, error) || error)
        {
            return MakePathValidationError(
                PathValidationErrorCode::InvalidRoot,
                error ? "Cannot inspect the project root: " + error.message() : "Project root is not a directory.");
        }

        const std::filesystem::path candidate =
            std::filesystem::weakly_canonical(canonicalRoot / Utf8ToPath(normalizedPath), error);
        if (error)
        {
            return MakePathValidationError(
                PathValidationErrorCode::ResolutionFailed,
                "Cannot resolve the project path: " + error.message());
        }
        if (!IsWithinRoot(canonicalRoot, candidate))
        {
            return MakePathValidationError(
                PathValidationErrorCode::OutsideRoot,
                "Project path resolves outside the project root.");
        }

        // Keep containment checks in canonical project paths. Only the
        // resulting file-I/O path needs the Windows extended spelling.
#if defined(HE_PACKAGED_GAME)
        outFullPath = PlatformPaths::ToFileIoPath(candidate);
#else
        outFullPath = candidate;
#endif
        outNormalized = std::move(normalizedPath);
        return {};
    }

    std::string MakeProjectPathCacheKey(std::string_view normalizedPath)
    {
        std::string key(normalizedPath);
        for (char& character : key)
        {
            if (character == '\\')
                character = '/';
            else if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character - 'A' + 'a');
        }
        return key;
    }

    std::filesystem::path Utf8ToPath(std::string_view path)
    {
        std::u8string utf8Path;
        utf8Path.reserve(path.size());
        for (const char character : path)
            utf8Path.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
        return std::filesystem::path(utf8Path);
    }

    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8Path = path.generic_u8string();
        std::string result;
        result.reserve(utf8Path.size());
        for (const char8_t character : utf8Path)
            result.push_back(static_cast<char>(character));
        return result;
    }

    std::filesystem::path PathWithSuffix(
        std::filesystem::path path,
        std::string_view utf8Suffix)
    {
        path += Utf8ToPath(utf8Suffix);
        return path;
    }

}
