#include "Runtime/Platform/PlatformFileSystem.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>

namespace HEngine
{
    struct PlatformStableFileReadLease::State {};
    PlatformStableFileReadLease::PlatformStableFileReadLease() noexcept = default;
    PlatformStableFileReadLease::~PlatformStableFileReadLease() = default;
    PlatformStableFileReadLease::PlatformStableFileReadLease(PlatformStableFileReadLease&&) noexcept = default;
    PlatformStableFileReadLease& PlatformStableFileReadLease::operator=(PlatformStableFileReadLease&&) noexcept = default;
    bool PlatformStableFileReadLease::IsValid() const noexcept { return false; }

    bool PlatformFileSystem::AcquireStableFileReadLease(std::span<const std::filesystem::path>,
        PlatformStableFileReadLease& outLease, std::error_code& outError) noexcept
    {
        // Advisory POSIX locks cannot guarantee the deny-write/delete contract.
        // Host tools must compile an owned snapshot and verify its source hashes.
        outLease = PlatformStableFileReadLease{};
        outError = std::make_error_code(std::errc::operation_not_supported);
        return false;
    }

    bool PlatformFileSystem::ReadSharedTextTail(const std::filesystem::path& path,
        std::size_t maxBytes, std::string& outText) noexcept
    {
        try
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) return false;
            const auto length = stream.tellg();
            if (length < 0) return false;
            const auto size = std::min<std::size_t>(static_cast<std::size_t>(length), maxBytes);
            outText.resize(size);
            stream.seekg(length - static_cast<std::streamoff>(size));
            stream.read(outText.data(), static_cast<std::streamsize>(size));
            outText.resize(static_cast<std::size_t>(stream.gcount()));
            return !stream.bad();
        }
        catch (...) { return false; }
    }

    std::filesystem::path PlatformFileSystem::MakeTemporarySiblingPath(const std::filesystem::path& targetPath)
    {
        static std::atomic<std::uint64_t> sequence = 0;
        return targetPath.parent_path() / (targetPath.filename().string() + ".tmp-" +
            std::to_string(getpid()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++sequence));
    }

    bool PlatformFileSystem::PublishFileAtomically(const std::filesystem::path& temporaryPath,
        const std::filesystem::path& targetPath, bool bReplaceExisting, std::error_code& outError) noexcept
    {
        if (bReplaceExisting)
        {
            if (::rename(temporaryPath.c_str(), targetPath.c_str()) == 0)
            {
                outError.clear();
                return true;
            }
        }
        else if (::link(temporaryPath.c_str(), targetPath.c_str()) == 0)
        {
            // Both names refer to the published inode until the temporary name
            // is removed. A failed cleanup does not undo successful publication.
            (void)::unlink(temporaryPath.c_str());
            outError.clear();
            return true;
        }
        outError = std::error_code(errno, std::generic_category());
        return false;
    }
}
