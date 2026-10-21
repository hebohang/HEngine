#include "AndroidContext.h"
#include "Runtime/Platform/PlatformPaths.h"
#include "Runtime/Platform/PlatformFileSystem.h"
#include "Runtime/Platform/PlatformProcess.h"
#include "Runtime/Profile/CpuExecutionTimes.h"

#include <android/log.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <fstream>

namespace HEngine
{
    std::filesystem::path PlatformPaths::GetExecutableDirectory() { return GetAndroidContext().ContentRoot; }
    std::filesystem::path PlatformPaths::GetLocalAppDataDirectory() { return GetAndroidContext().SavedRoot; }
    std::filesystem::path PlatformPaths::ToFileIoPath(const std::filesystem::path& path) { return path; }
    void PlatformPaths::ShowFatalDialog(std::string_view title, std::string_view message) noexcept
    {
        try
        {
            auto& state = GetAndroidContext();
            std::lock_guard lock(state.Mutex);
            state.Error = std::string(title) + ": " + std::string(message);
        }
        catch (...) {}
        __android_log_print(ANDROID_LOG_ERROR, "HEngine", "%.*s: %.*s",
            int(title.size()), title.data(), int(message.size()), message.data());
    }
    struct PlatformStableFileReadLease::State {};
    PlatformStableFileReadLease::PlatformStableFileReadLease() noexcept = default;
    PlatformStableFileReadLease::~PlatformStableFileReadLease() = default;
    PlatformStableFileReadLease::PlatformStableFileReadLease(PlatformStableFileReadLease&&) noexcept = default;
    PlatformStableFileReadLease& PlatformStableFileReadLease::operator=(PlatformStableFileReadLease&&) noexcept = default;
    bool PlatformStableFileReadLease::IsValid() const noexcept { return false; }
    bool PlatformFileSystem::AcquireStableFileReadLease(std::span<const std::filesystem::path>,
        PlatformStableFileReadLease&, std::error_code& error) noexcept
    {
        // POSIX advisory locks cannot promise Windows deny-write semantics.
        // This capability is used only by host shader compilation tools.
        error = std::make_error_code(std::errc::operation_not_supported);
        return false;
    }
    bool PlatformFileSystem::ReadSharedTextTail(const std::filesystem::path& path,
        std::size_t maxBytes, std::string& output) noexcept
    {
        try
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) return false;
            const auto length = stream.tellg();
            if (length < 0) return false;
            const auto size = std::min<std::size_t>(static_cast<std::size_t>(length), maxBytes);
            output.resize(size);
            stream.seekg(length - static_cast<std::streamoff>(size));
            stream.read(output.data(), size);
            output.resize(static_cast<size_t>(stream.gcount()));
            return !stream.bad();
        }
        catch (...) { return false; }
    }
    std::filesystem::path PlatformFileSystem::MakeTemporarySiblingPath(const std::filesystem::path& target)
    {
        static std::atomic<std::uint64_t> sequence = 0;
        return target.parent_path() / (target.filename().string() + ".tmp-" +
            std::to_string(getpid()) + "-" + std::to_string(++sequence));
    }
    bool PlatformFileSystem::PublishFileAtomically(const std::filesystem::path& temporary,
        const std::filesystem::path& target, bool replace, std::error_code& error) noexcept
    {
        if (replace)
        {
            if (rename(temporary.c_str(), target.c_str()) == 0) { error.clear(); return true; }
        }
        else if (link(temporary.c_str(), target.c_str()) == 0)
        {
            unlink(temporary.c_str()); error.clear(); return true;
        }
        error = std::error_code(errno, std::generic_category());
        return false;
    }
    PlatformProcessMemorySnapshot PlatformProcess::ReadCurrentMemory() noexcept { return {}; }
    PlatformProcess::CapturedResult PlatformProcess::RunCaptured(const std::filesystem::path&,
        std::span<const std::string>, std::stop_token, unsigned)
    {
        return {-1, false, "Launching desktop tools is unavailable on Android"};
    }
    PlatformProcessLaunchResult PlatformProcess::RelaunchCurrentProcess(std::span<const std::string>) noexcept
    {
        return {false, "Android Activity owns process lifecycle"};
    }
    CpuExecutionTimes QueryCpuExecutionTimes() noexcept
    {
        timespec thread{}, process{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &thread) || clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &process)) return {};
        const auto ticks = [](const timespec& t) { return uint64_t(t.tv_sec) * 10000000 + uint64_t(t.tv_nsec) / 100; };
        return {uint64_t(syscall(SYS_gettid)), ticks(thread), ticks(process), true};
    }
}
