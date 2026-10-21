#include "Runtime/Platform/PlatformProcess.h"
#include "Runtime/Profile/CpuExecutionTimes.h"

#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace HEngine
{
    namespace
    {
        struct FileDescriptor
        {
            int Value = -1;
            ~FileDescriptor() { if (Value >= 0) ::close(Value); }
            void Close() { if (Value >= 0) ::close(Value); Value = -1; }
        };

        struct SpawnActions
        {
            posix_spawn_file_actions_t Value;
            SpawnActions()
            {
                const int error = posix_spawn_file_actions_init(&Value);
                if (error) throw std::system_error(error, std::generic_category());
            }
            ~SpawnActions() { posix_spawn_file_actions_destroy(&Value); }
        };

        struct SpawnAttributes
        {
            posix_spawnattr_t Value;
            SpawnAttributes()
            {
                const int error = posix_spawnattr_init(&Value);
                if (error) throw std::system_error(error, std::generic_category());
            }
            ~SpawnAttributes() { posix_spawnattr_destroy(&Value); }
        };

        void CheckSpawn(int error)
        {
            if (error) throw std::system_error(error, std::generic_category());
        }

        std::vector<char*> MakeArguments(std::string& executable, std::vector<std::string>& arguments)
        {
            std::vector<char*> result{executable.data()};
            result.reserve(arguments.size() + 2);
            for (auto& argument : arguments) result.push_back(argument.data());
            result.push_back(nullptr);
            return result;
        }
    }

    PlatformProcessMemorySnapshot PlatformProcess::ReadCurrentMemory() noexcept
    {
        PlatformProcessMemorySnapshot result;
        task_vm_info_data_t info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        const auto status = task_info(mach_task_self(), TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&info), &count);
        result.ProcessError = static_cast<std::uint32_t>(status);
        if (status == KERN_SUCCESS)
        {
            result.bProcessAvailable = true;
            result.PrivateBytes = info.phys_footprint;
            result.WorkingSetBytes = info.resident_size;
        }
        // Darwin has no Win32-style default process heap accounting.
        return result;
    }

    CpuExecutionTimes QueryCpuExecutionTimes() noexcept
    {
        timespec threadTime{}, processTime{};
        std::uint64_t threadId = 0;
        if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &threadTime) != 0 ||
            ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &processTime) != 0 ||
            ::pthread_threadid_np(nullptr, &threadId) != 0)
            return {};
        const auto ticks = [](const timespec& value)
        {
            return std::uint64_t(value.tv_sec) * 10000000u + std::uint64_t(value.tv_nsec) / 100u;
        };
        return {threadId, ticks(threadTime), ticks(processTime), true};
    }

    PlatformProcess::CapturedResult PlatformProcess::RunCaptured(const std::filesystem::path& executable,
        std::span<const std::string> arguments, std::stop_token stop, unsigned timeoutSeconds)
    {
        CapturedResult result;
        if (stop.stop_requested()) return {-1, true, "Compiler cancelled before launch."};
        try
        {
            if (executable.empty()) throw std::invalid_argument("Captured process executable is empty");
            int pipeEnds[2];
            if (::pipe(pipeEnds) != 0) throw std::system_error(errno, std::generic_category());
            FileDescriptor read{pipeEnds[0]}, write{pipeEnds[1]};
            for (const int descriptor : pipeEnds)
                if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0)
                    throw std::system_error(errno, std::generic_category());
            if (::fcntl(read.Value, F_SETFL, O_NONBLOCK) < 0)
                throw std::system_error(errno, std::generic_category());

            SpawnActions actions;
            CheckSpawn(posix_spawn_file_actions_adddup2(&actions.Value, write.Value, STDOUT_FILENO));
            CheckSpawn(posix_spawn_file_actions_adddup2(&actions.Value, write.Value, STDERR_FILENO));
            CheckSpawn(posix_spawn_file_actions_addopen(&actions.Value, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
            CheckSpawn(posix_spawn_file_actions_addclose(&actions.Value, read.Value));
            CheckSpawn(posix_spawn_file_actions_addclose(&actions.Value, write.Value));
            SpawnAttributes attributes;
            CheckSpawn(posix_spawnattr_setflags(&attributes.Value, POSIX_SPAWN_SETPGROUP));
            CheckSpawn(posix_spawnattr_setpgroup(&attributes.Value, 0));
            std::string nativeExecutable = executable.string();
            std::vector<std::string> ownedArguments(arguments.begin(), arguments.end());
            auto nativeArguments = MakeArguments(nativeExecutable, ownedArguments);
            pid_t pid = -1;
            CheckSpawn(::posix_spawnp(&pid, nativeExecutable.c_str(), &actions.Value,
                &attributes.Value, nativeArguments.data(), *_NSGetEnviron()));
            write.Close();

            // The process group belongs only to this invocation. Cleanup never
            // enumerates or signals unrelated editor/user processes.
            struct ChildScope
            {
                pid_t Pid;
                bool Reaped = false;
                ~ChildScope()
                {
                    ::kill(-Pid, SIGKILL);
                    if (!Reaped)
                    {
                        int status;
                        while (::waitpid(Pid, &status, 0) < 0 && errno == EINTR) {}
                    }
                }
            } child{pid};
            constexpr std::size_t outputLimit = 1024 * 1024;
            std::array<char, 8192> buffer{};
            const auto started = std::chrono::steady_clock::now();
            int status = 0;
            bool terminating = false;
            auto terminateStarted = started;
            while (true)
            {
                for (unsigned reads = 0; reads < 8; ++reads)
                {
                    const auto bytes = ::read(read.Value, buffer.data(), buffer.size());
                    if (bytes > 0)
                    {
                        result.Output.append(buffer.data(), static_cast<std::size_t>(bytes));
                        if (result.Output.size() > outputLimit)
                            result.Output.erase(0, result.Output.size() - outputLimit);
                    }
                    else if (bytes < 0 && errno == EINTR) continue;
                    else break;
                }
                const auto waited = ::waitpid(pid, &status, WNOHANG);
                if (waited == pid) { child.Reaped = true; break; }
                if (waited < 0 && errno != EINTR)
                    throw std::system_error(errno, std::generic_category());
                const auto now = std::chrono::steady_clock::now();
                const bool timedOut = now - started >= std::chrono::seconds(timeoutSeconds);
                if (!terminating && (stop.stop_requested() || timedOut))
                {
                    result.Cancelled = true;
                    terminating = true;
                    terminateStarted = now;
                    ::kill(-pid, SIGTERM);
                }
                else if (terminating && now - terminateStarted >= std::chrono::milliseconds(250))
                    ::kill(-pid, SIGKILL);
                pollfd descriptor{read.Value, POLLIN, 0};
                (void)::poll(&descriptor, 1, 20);
            }
            ::kill(-pid, SIGKILL);
            // Read the final bytes already in the pipe without waiting for any
            // descendants that inherited stdout to keep it open.
            for (;;)
            {
                const auto bytes = ::read(read.Value, buffer.data(), buffer.size());
                if (bytes > 0)
                {
                    result.Output.append(buffer.data(), static_cast<std::size_t>(bytes));
                    if (result.Output.size() > outputLimit)
                        result.Output.erase(0, result.Output.size() - outputLimit);
                }
                else if (bytes < 0 && errno == EINTR) continue;
                else break;
            }
            result.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) :
                (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
            if (result.Cancelled) result.Output += "\nCompiler cancelled or timed out.\n";
        }
        catch (const std::exception& error) { result.Output += error.what(); }
        if (result.Output.size() > 1024 * 1024)
            result.Output.erase(0, result.Output.size() - 1024 * 1024);
        return result;
    }

    PlatformProcessLaunchResult PlatformProcess::RelaunchCurrentProcess(
        std::span<const std::string> arguments) noexcept
    {
        try
        {
            std::uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::string path(size, '\0');
            if (_NSGetExecutablePath(path.data(), &size) != 0)
                return {false, "Cannot resolve the executable path"};
            path.resize(std::char_traits<char>::length(path.c_str()));
            std::vector<std::string> ownedArguments(arguments.begin(), arguments.end());
            auto nativeArguments = MakeArguments(path, ownedArguments);
            pid_t pid;
            const auto error = ::posix_spawn(&pid, path.c_str(), nullptr, nullptr,
                nativeArguments.data(), *_NSGetEnviron());
            return error ? PlatformProcessLaunchResult{false, std::system_category().message(error)} :
                PlatformProcessLaunchResult{true, {}};
        }
        catch (const std::exception& error) { return {false, error.what()}; }
        catch (...) { return {false, "Cannot relaunch the process"}; }
    }
}
