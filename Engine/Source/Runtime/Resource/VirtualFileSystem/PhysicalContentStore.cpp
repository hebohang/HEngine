#include "Hepch.h"

#include "Runtime/Resource/VirtualFileSystem/PhysicalContentStore.h"

#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr std::size_t ContentReadChunkBytes = 64u * 1024u;

        [[nodiscard]] std::filesystem::path CaptureProjectRoot(
            const std::filesystem::path& projectRoot)
        {
            if (projectRoot.empty())
                throw std::invalid_argument("Physical content root cannot be empty.");

            std::error_code error;
            const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, error);
            if (error)
            {
                throw std::invalid_argument(
                    "Cannot make the physical content root absolute: " + error.message());
            }

            const std::filesystem::path canonicalRoot =
                std::filesystem::weakly_canonical(absoluteRoot, error);
            if (error)
            {
                throw std::invalid_argument(
                    "Cannot resolve the physical content root: " + error.message());
            }
            if (!std::filesystem::is_directory(canonicalRoot, error) || error)
            {
                throw std::invalid_argument(
                    error
                        ? "Cannot inspect the physical content root: " + error.message()
                        : "Physical content root is not a directory.");
            }
            return canonicalRoot;
        }

        [[noreturn]] void ThrowContentReadCancelled(std::string_view projectPath)
        {
            throw std::runtime_error(
                "Content read was cancelled: " + std::string(projectPath));
        }

        void CheckContentReadCancelled(
            std::string_view projectPath,
            std::stop_token stopToken)
        {
            if (stopToken.stop_requested())
                ThrowContentReadCancelled(projectPath);
        }

        [[nodiscard]] bool IsMissingFileError(const std::error_code& error) noexcept
        {
            return error == std::errc::no_such_file_or_directory ||
                error == std::errc::not_a_directory;
        }

        [[nodiscard]] bool IsWithinCapturedRoot(
            const std::filesystem::path& capturedRoot,
            const std::filesystem::path& candidate)
        {
            const std::filesystem::path relative = candidate.lexically_relative(capturedRoot);
            if (relative.empty())
                return candidate == capturedRoot;
            if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
                return false;
            return std::none_of(relative.begin(), relative.end(), [](const std::filesystem::path& component)
            {
                return component == "..";
            });
        }
    }

    PhysicalContentStore::PhysicalContentStore(std::filesystem::path projectRoot)
        : mProjectRoot(CaptureProjectRoot(projectRoot))
    {
    }

    std::vector<std::uint8_t> PhysicalContentStore::ReadAll(
        std::string_view projectPath,
        std::size_t maxBytes,
        std::stop_token stopToken) const
    {
        CheckContentReadCancelled(projectPath, stopToken);
        const std::filesystem::path fullPath = Resolve(projectPath);

        std::error_code error;
        if (!std::filesystem::is_regular_file(fullPath, error) || error)
        {
            throw std::runtime_error(
                error
                    ? "Cannot inspect physical content file '" + std::string(projectPath) +
                        "': " + error.message()
                    : "Physical content path is not a regular file: " + std::string(projectPath));
        }

        CheckContentReadCancelled(projectPath, stopToken);
        std::ifstream file(fullPath, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error(
                "Failed to open physical content file: " + std::string(projectPath));
        }

        std::array<std::uint8_t, ContentReadChunkBytes> chunk{};
        std::vector<std::uint8_t> bytes;
        bytes.reserve(std::min(maxBytes, chunk.size()));

        while (true)
        {
            CheckContentReadCancelled(projectPath, stopToken);
            const std::size_t remaining = maxBytes - bytes.size();
            const std::size_t requestedBytes = remaining >= chunk.size()
                ? chunk.size()
                : remaining + 1;

            file.read(
                reinterpret_cast<char*>(chunk.data()),
                static_cast<std::streamsize>(requestedBytes));
            const std::streamsize readCount = file.gcount();
            if (readCount > 0)
            {
                const std::size_t readBytes = static_cast<std::size_t>(readCount);
                bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + readBytes);
                if (bytes.size() > maxBytes)
                {
                    throw std::runtime_error(
                        "Physical content file exceeds the configured byte limit: " +
                        std::string(projectPath));
                }
            }

            if (file.bad())
            {
                throw std::runtime_error(
                    "Failed to read physical content file: " + std::string(projectPath));
            }
            if (readCount == 0 || file.eof())
                break;
            if (file.fail())
            {
                throw std::runtime_error(
                    "Failed to read physical content file: " + std::string(projectPath));
            }
        }

        CheckContentReadCancelled(projectPath, stopToken);
        return bytes;
    }

    std::optional<std::uint64_t> PhysicalContentStore::TryGetSize(
        std::string_view projectPath) const
    {
        const std::filesystem::path fullPath = Resolve(projectPath);

        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::status(fullPath, error);
        if (error)
        {
            if (IsMissingFileError(error))
                return std::nullopt;
            throw std::runtime_error(
                "Cannot inspect physical content path '" + std::string(projectPath) +
                "': " + error.message());
        }
        if (!std::filesystem::is_regular_file(status))
            return std::nullopt;

        const std::uintmax_t size = std::filesystem::file_size(fullPath, error);
        if (error)
        {
            if (IsMissingFileError(error))
                return std::nullopt;
            throw std::runtime_error(
                "Cannot read physical content file size '" + std::string(projectPath) +
                "': " + error.message());
        }
        return static_cast<std::uint64_t>(size);
    }

    std::filesystem::path PhysicalContentStore::Resolve(std::string_view projectPath) const
    {
        std::string normalizedPath;
        const Utils::PathValidationResult result =
            Utils::TryNormalizeProjectRelativePath(projectPath, normalizedPath);
        if (!result)
        {
            throw std::invalid_argument(
                "Invalid physical content path: " + result.Message);
        }

        std::error_code error;
        const std::filesystem::path fullPath =
            std::filesystem::weakly_canonical(mProjectRoot / normalizedPath, error);
        if (error)
        {
            throw std::invalid_argument(
                "Cannot resolve physical content path: " + error.message());
        }
        if (!IsWithinCapturedRoot(mProjectRoot, fullPath))
        {
            throw std::invalid_argument(
                "Physical content path resolves outside the captured content root.");
        }
        return fullPath;
    }
}
