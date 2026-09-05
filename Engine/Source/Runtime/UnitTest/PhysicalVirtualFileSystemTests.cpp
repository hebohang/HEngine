#include "Hepch.h"

#include "Runtime/UnitTest/PhysicalVirtualFileSystemTests.h"
#include "Runtime/Resource/VirtualFileSystem/PhysicalContentStore.h"
#include "Runtime/Resource/VirtualFileSystem/VirtualFileSystem.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        class ScopedTempDirectory final
        {
        public:
            ScopedTempDirectory()
            {
                static std::atomic_uint64_t sSuffix = 0;
                const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                mPath = std::filesystem::temp_directory_path() /
                    ("HEngine_PhysicalVfs_" + std::to_string(timestamp) + "_" +
                        std::to_string(sSuffix.fetch_add(1, std::memory_order_relaxed)));
                std::filesystem::create_directories(mPath);
            }

            ~ScopedTempDirectory()
            {
                std::error_code error;
                std::filesystem::remove_all(mPath, error);
            }

            [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
            {
                return mPath;
            }

        private:
            std::filesystem::path mPath;
        };

        class ScopedVirtualFileSystemSnapshot final
        {
        public:
            ScopedVirtualFileSystemSnapshot()
                : mSnapshot(VirtualFileSystem::GetInstance().Capture())
            {
            }

            ~ScopedVirtualFileSystemSnapshot()
            {
                VirtualFileSystem::GetInstance().Install(std::move(mSnapshot));
            }

        private:
            ContentStoreSnapshot mSnapshot;
        };

        void WriteBytes(
            const std::filesystem::path& path,
            const std::vector<std::uint8_t>& bytes)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("Could not create a physical VFS test fixture.");
            if (!bytes.empty())
            {
                file.write(
                    reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
            }
            if (!file)
                throw std::runtime_error("Could not write a physical VFS test fixture.");
        }

        template<typename Callback>
        void RequireInvalidPath(Callback&& callback, std::string_view message)
        {
            bool bRejected = false;
            try
            {
                std::forward<Callback>(callback)();
            }
            catch (const std::invalid_argument&)
            {
                bRejected = true;
            }
            Require(bRejected, message);
        }

        template<typename Callback>
        void RequireRuntimeFailure(Callback&& callback, std::string_view message)
        {
            bool bFailed = false;
            try
            {
                std::forward<Callback>(callback)();
            }
            catch (const std::runtime_error&)
            {
                bFailed = true;
            }
            Require(bFailed, message);
        }

        [[nodiscard]] bool IsExpectedSymlinkCreationFailure(
            const std::error_code& error) noexcept
        {
            return error == std::errc::permission_denied ||
                error == std::errc::operation_not_permitted ||
                error == std::errc::function_not_supported;
        }

        void VerifyPhysicalParityAliasesAndStrictPaths()
        {
            ScopedTempDirectory temporaryDirectory;
            const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
            const std::filesystem::path fixturePath = projectRoot / "Assets/Nested/Fixture.bin";
            const std::vector<std::uint8_t> expected{ 0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff, 0x42 };
            WriteBytes(fixturePath, expected);
            std::filesystem::create_directories(projectRoot / "Assets/DirectoryOnly");

            PhysicalContentStore store(projectRoot);
            const std::vector<std::uint8_t> direct =
                store.ReadAll("Assets/Nested/Fixture.bin", expected.size());
            Require(direct == expected,
                "Physical content bytes did not match the source file.");

            const std::vector<std::uint8_t> alias =
                store.ReadAll("assets\\.\\nested\\FIXTURE.bin", expected.size());
            Require(alias == expected,
                "A case/separator/dot alias did not resolve to identical physical content.");

            const auto size = store.TryGetSize("Assets\\Nested\\Fixture.bin");
            Require(size && *size == expected.size(),
                "Physical content size did not match the source file.");
            Require(!store.TryGetSize("Assets/Missing.bin"),
                "A missing physical content path reported a size.");
            Require(!store.TryGetSize("Assets/DirectoryOnly"),
                "A physical content directory reported a file size.");
            RequireRuntimeFailure([&]
            {
                (void)store.ReadAll("Assets/Missing.bin", 1);
            }, "Reading missing physical content did not fail.");

            std::vector<std::string> invalidPaths{
                "",
                "../Outside.bin",
                "Assets/../Outside.bin",
                "C:/Windows/win.ini",
                "\\\\server\\share\\payload.bin",
                "Assets/CON/payload.bin",
                "Assets/Bad./payload.bin"
            };
            invalidPaths.emplace_back("Assets/Bad\0Name.bin", 19);
            for (const std::string& invalidPath : invalidPaths)
            {
                RequireInvalidPath([&]
                {
                    (void)store.TryGetSize(invalidPath);
                }, "TryGetSize accepted an unsafe project path.");
                RequireInvalidPath([&]
                {
                    (void)store.ReadAll(invalidPath, 1);
                }, "ReadAll accepted an unsafe project path.");
            }
        }

        void VerifyOutsideRootSymlinkRejection()
        {
            ScopedTempDirectory temporaryDirectory;
            const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
            const std::filesystem::path outsidePath = temporaryDirectory.GetPath() / "Outside/Payload.bin";
            const std::filesystem::path linkPath = projectRoot / "Assets/Escape.bin";
            WriteBytes(outsidePath, { 0x31, 0x32, 0x33 });
            std::filesystem::create_directories(linkPath.parent_path());

            std::error_code error;
            std::filesystem::create_symlink(outsidePath, linkPath, error);
            if (error)
            {
                Require(IsExpectedSymlinkCreationFailure(error),
                    "The outside-root symlink fixture failed for an unexpected reason.");
                return;
            }

            PhysicalContentStore store(projectRoot);
            RequireInvalidPath([&]
            {
                (void)store.TryGetSize("Assets/Escape.bin");
            }, "TryGetSize followed a symlink outside the physical content root.");
            RequireInvalidPath([&]
            {
                (void)store.ReadAll("Assets/Escape.bin", 3);
            }, "ReadAll followed a symlink outside the physical content root.");
        }

        void VerifyCapturedRootCannotDriftThroughReplacement()
        {
            ScopedTempDirectory temporaryDirectory;
            const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
            const std::filesystem::path movedRoot = temporaryDirectory.GetPath() / "MovedProject";
            const std::filesystem::path outsideRoot = temporaryDirectory.GetPath() / "Outside";
            WriteBytes(projectRoot / "Assets/Original.bin", { 0x01 });
            WriteBytes(outsideRoot / "Assets/Escape.bin", { 0x02 });

            PhysicalContentStore store(projectRoot);
            std::filesystem::rename(projectRoot, movedRoot);

            std::error_code error;
            std::filesystem::create_directory_symlink(outsideRoot, projectRoot, error);
            if (error)
            {
                Require(IsExpectedSymlinkCreationFailure(error),
                    "The physical-root replacement fixture failed for an unexpected reason.");
                return;
            }

            RequireInvalidPath([&]
            {
                (void)store.TryGetSize("Assets/Escape.bin");
            }, "TryGetSize accepted a content root replaced by an outside symlink.");
            RequireInvalidPath([&]
            {
                (void)store.ReadAll("Assets/Escape.bin", 1);
            }, "ReadAll accepted a content root replaced by an outside symlink.");
        }

        void VerifyBoundedReadAndCancellation()
        {
            ScopedTempDirectory temporaryDirectory;
            const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
            constexpr std::size_t Limit = 64u * 1024u;
            std::vector<std::uint8_t> payload(Limit + 1);
            for (std::size_t index = 0; index < payload.size(); ++index)
                payload[index] = static_cast<std::uint8_t>(index % 251u);
            WriteBytes(projectRoot / "Assets/Bounded.bin", payload);

            PhysicalContentStore store(projectRoot);
            const std::vector<std::uint8_t> exact =
                store.ReadAll("Assets/Bounded.bin", payload.size());
            Require(exact == payload,
                "An exact-bound physical content read changed the payload.");
            RequireRuntimeFailure([&]
            {
                (void)store.ReadAll("Assets/Bounded.bin", Limit);
            }, "A maxBytes+1 physical content file bypassed the read bound.");

            std::stop_source stopSource;
            stopSource.request_stop();
            RequireRuntimeFailure([&]
            {
                (void)store.ReadAll(
                    "Assets/Bounded.bin",
                    std::numeric_limits<std::size_t>::max(),
                    stopSource.get_token());
            }, "A pre-cancelled physical content read completed successfully.");
        }

        void VerifyAtomicVirtualFileSystemSnapshots()
        {
            ScopedTempDirectory temporaryDirectory;
            const std::filesystem::path firstRoot = temporaryDirectory.GetPath() / "First";
            const std::filesystem::path secondRoot = temporaryDirectory.GetPath() / "Second";
            const std::vector<std::uint8_t> firstBytes{ 'f', 'i', 'r', 's', 't' };
            const std::vector<std::uint8_t> secondBytes{ 's', 'e', 'c', 'o', 'n', 'd' };
            WriteBytes(firstRoot / "Assets/Shared.bin", firstBytes);
            WriteBytes(secondRoot / "Assets/Shared.bin", secondBytes);

            const ContentStoreSnapshot first = std::make_shared<PhysicalContentStore>(firstRoot);
            const ContentStoreSnapshot second = std::make_shared<PhysicalContentStore>(secondRoot);
            VirtualFileSystem& fileSystem = VirtualFileSystem::GetInstance();
            ScopedVirtualFileSystemSnapshot restoreSnapshot;
            fileSystem.Clear();
            Require(!fileSystem.Capture(),
                "Clearing the virtual file system did not publish an empty snapshot.");
            fileSystem.Install({});
            Require(!fileSystem.Capture(),
                "Installing a null content store did not publish an empty snapshot.");
            fileSystem.Install(first);
            Require(fileSystem.Capture() == first,
                "Installing a content store did not publish that exact snapshot.");

            constexpr std::size_t ReaderCount = 4;
            std::atomic_size_t readyCount = 0;
            std::atomic_size_t observationCount = 0;
            std::atomic_bool bStart = false;
            std::atomic_bool bStop = false;
            std::atomic_bool bObservedInvalidSnapshot = false;
            std::vector<std::thread> readers;
            readers.reserve(ReaderCount);
            for (std::size_t index = 0; index < ReaderCount; ++index)
            {
                readers.emplace_back([&]
                {
                    readyCount.fetch_add(1, std::memory_order_release);
                    while (!bStart.load(std::memory_order_acquire))
                        std::this_thread::yield();

                    do
                    {
                        const ContentStoreSnapshot observed = fileSystem.Capture();
                        if (!observed)
                            continue;

                        const std::vector<std::uint8_t>* expected = nullptr;
                        if (observed == first)
                            expected = &firstBytes;
                        else if (observed == second)
                            expected = &secondBytes;
                        else
                        {
                            bObservedInvalidSnapshot.store(true, std::memory_order_relaxed);
                            continue;
                        }

                        try
                        {
                            if (observed->ReadAll("Assets/Shared.bin", expected->size()) != *expected)
                                bObservedInvalidSnapshot.store(true, std::memory_order_relaxed);
                        }
                        catch (...)
                        {
                            bObservedInvalidSnapshot.store(true, std::memory_order_relaxed);
                        }
                        observationCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    while (!bStop.load(std::memory_order_acquire));
                });
            }

            while (readyCount.load(std::memory_order_acquire) != ReaderCount)
                std::this_thread::yield();
            bStart.store(true, std::memory_order_release);
            for (std::size_t iteration = 0; iteration < 8192; ++iteration)
            {
                if (iteration % 3 == 0)
                    fileSystem.Install(first);
                else if (iteration % 3 == 1)
                    fileSystem.Install(second);
                else
                    fileSystem.Clear();
                if ((iteration & 63u) == 0)
                    std::this_thread::yield();
            }
            bStop.store(true, std::memory_order_release);
            for (std::thread& reader : readers)
                reader.join();

            Require(observationCount.load(std::memory_order_relaxed) != 0,
                "Concurrent VFS readers did not observe an installed snapshot.");
            Require(!bObservedInvalidSnapshot.load(std::memory_order_relaxed),
                "A concurrent VFS reader observed a torn, unknown, or mismatched snapshot.");

            fileSystem.Clear();
            Require(!fileSystem.Capture(),
                "The concurrent VFS fixture did not finish on an empty snapshot.");
        }
    }

    void PhysicalVirtualFileSystemContract()
    {
        VerifyPhysicalParityAliasesAndStrictPaths();
        VerifyOutsideRootSymlinkRejection();
        VerifyCapturedRootCannotDriftThroughReplacement();
        VerifyBoundedReadAndCancellation();
        VerifyAtomicVirtualFileSystemSnapshots();
    }
}
