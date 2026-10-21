#include "Runtime/Platform/PlatformFileSystem.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace HEngine
{
    bool PlatformFileSystem::ReadSharedTextTail(const std::filesystem::path& path,
        std::size_t maxBytes, std::string& outText) noexcept
    {
        if (maxBytes == 0 || maxBytes > 1024 * 1024) return false;
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        struct CloseFile { HANDLE File; ~CloseFile() { ::CloseHandle(File); } } close{file};
        try
        {
            LARGE_INTEGER size{}, offset{};
            if (!::GetFileSizeEx(file, &size)) return false;
            offset.QuadPart = size.QuadPart > static_cast<LONGLONG>(maxBytes) ? size.QuadPart - maxBytes : 0;
            if (!::SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) return false;
            std::string text(maxBytes, '\0'); DWORD read = 0;
            if (!::ReadFile(file, text.data(), static_cast<DWORD>(maxBytes), &read, nullptr)) return false;
            text.resize(read);
            if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
            while (!text.empty() && (static_cast<unsigned char>(text.front()) & 0xc0) == 0x80) text.erase(0, 1);
            outText = std::move(text); return true;
        }
        catch (...) { return false; }
    }

    struct PlatformStableFileReadLease::State final
    {
        ~State()
        {
            for (const HANDLE handle : Handles)
            {
                if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
                    ::CloseHandle(handle);
            }
        }

        std::vector<HANDLE> Handles;
    };

    PlatformStableFileReadLease::PlatformStableFileReadLease() noexcept = default;

    PlatformStableFileReadLease::~PlatformStableFileReadLease() = default;

    PlatformStableFileReadLease::PlatformStableFileReadLease(
        PlatformStableFileReadLease&& other) noexcept = default;

    PlatformStableFileReadLease& PlatformStableFileReadLease::operator=(
        PlatformStableFileReadLease&& other) noexcept = default;

    bool PlatformStableFileReadLease::IsValid() const noexcept
    {
        return mState != nullptr;
    }

    std::filesystem::path PlatformFileSystem::MakeTemporarySiblingPath(
        const std::filesystem::path& targetPath)
    {
        static std::atomic_uint64_t sTemporaryFileCounter = 0;

        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::uint64_t counter = sTemporaryFileCounter.fetch_add(1, std::memory_order_relaxed);
        return targetPath.parent_path() /
            (targetPath.filename().wstring() + L".tmp." +
            std::to_wstring(::GetCurrentProcessId()) + L"." +
            std::to_wstring(timestamp) + L"." + std::to_wstring(counter));
    }

    bool PlatformFileSystem::PublishFileAtomically(
        const std::filesystem::path& temporaryPath,
        const std::filesystem::path& targetPath,
        bool bReplaceExisting,
        std::error_code& outError) noexcept
    {
        const DWORD flags = MOVEFILE_WRITE_THROUGH |
            (bReplaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
        if (::MoveFileExW(
                temporaryPath.c_str(),
                targetPath.c_str(),
                flags) != 0)
        {
            outError.clear();
            return true;
        }

        outError = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return false;
    }

    bool PlatformFileSystem::AcquireStableFileReadLease(
        std::span<const std::filesystem::path> filePaths,
        PlatformStableFileReadLease& outLease,
        std::error_code& outError) noexcept
    {
        outLease = PlatformStableFileReadLease{};
        try
        {
            auto state = std::make_unique<PlatformStableFileReadLease::State>();
            state->Handles.reserve(filePaths.size());
            for (const std::filesystem::path& filePath : filePaths)
            {
                // Omitting FILE_SHARE_WRITE and FILE_SHARE_DELETE makes both
                // in-place saves and temp-file replace/rename saves fail until
                // the lease is released. Multiple readers/compilers remain
                // compatible through FILE_SHARE_READ.
                const HANDLE handle = ::CreateFileW(
                    filePath.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                {
                    outError = std::error_code(
                        static_cast<int>(::GetLastError()), std::system_category());
                    return false;
                }

                BY_HANDLE_FILE_INFORMATION fileInformation{};
                if (::GetFileInformationByHandle(handle, &fileInformation) == 0)
                {
                    const DWORD error = ::GetLastError();
                    ::CloseHandle(handle);
                    outError = std::error_code(
                        static_cast<int>(error), std::system_category());
                    return false;
                }
                if ((fileInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    ::CloseHandle(handle);
                    outError = std::error_code(ERROR_DIRECTORY, std::system_category());
                    return false;
                }
                state->Handles.push_back(handle);
            }

            outLease.mState = std::move(state);
            outError.clear();
            return true;
        }
        catch (const std::bad_alloc&)
        {
            outError = std::make_error_code(std::errc::not_enough_memory);
            return false;
        }
        catch (...)
        {
            outError = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
}
