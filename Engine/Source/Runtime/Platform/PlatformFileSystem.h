#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <system_error>

namespace HEngine
{
    // Holds read handles whose platform sharing mode prevents the leased files
    // from being modified, deleted, renamed, or atomically replaced. The lease
    // is intentionally opaque so native handle types remain in Platform/.
    class PlatformStableFileReadLease final
    {
    public:
        PlatformStableFileReadLease() noexcept;
        ~PlatformStableFileReadLease();

        PlatformStableFileReadLease(PlatformStableFileReadLease&& other) noexcept;
        PlatformStableFileReadLease& operator=(PlatformStableFileReadLease&& other) noexcept;

        PlatformStableFileReadLease(const PlatformStableFileReadLease&) = delete;
        PlatformStableFileReadLease& operator=(const PlatformStableFileReadLease&) = delete;

        [[nodiscard]] bool IsValid() const noexcept;

    private:
        friend class PlatformFileSystem;

        struct State;
        std::unique_ptr<State> mState;
    };

    class PlatformFileSystem final
    {
    public:
        [[nodiscard]] static std::filesystem::path MakeTemporarySiblingPath(
            const std::filesystem::path& targetPath);

        [[nodiscard]] static bool PublishFileAtomically(
            const std::filesystem::path& temporaryPath,
            const std::filesystem::path& targetPath,
            // False makes publication fail if the target already exists.
            bool bReplaceExisting,
            std::error_code& outError) noexcept;

        [[nodiscard]] static bool AcquireStableFileReadLease(
            std::span<const std::filesystem::path> filePaths,
            PlatformStableFileReadLease& outLease,
            std::error_code& outError) noexcept;
    };
}
