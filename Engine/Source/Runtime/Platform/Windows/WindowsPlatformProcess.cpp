#include "Runtime/Platform/PlatformProcess.h"

#include <Windows.h>
#include <Psapi.h>

#include <filesystem>
#include <string_view>
#include <vector>
#include <chrono>
#include <memory>
#include <algorithm>
#include <stdexcept>

namespace HEngine
{
    PlatformProcessMemorySnapshot PlatformProcess::ReadCurrentMemory() noexcept
    {
        PlatformProcessMemorySnapshot result;
        PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb = sizeof(process);
        result.bProcessAvailable = ::K32GetProcessMemoryInfo(::GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)) != FALSE;
        if (result.bProcessAvailable)
        {
            result.PrivateBytes = process.PrivateUsage;
            result.WorkingSetBytes = process.WorkingSetSize;
        }
        else
            result.ProcessError = ::GetLastError();

        // Resolve dynamically: older Windows versions must remain loadable.
        using HeapSummaryFunction = BOOL(WINAPI*)(HANDLE, DWORD, LPHEAP_SUMMARY);
        static const auto heapSummary = reinterpret_cast<HeapSummaryFunction>(
            ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "HeapSummary"));
        if (!heapSummary)
        {
            result.DefaultHeapError = ERROR_PROC_NOT_FOUND;
            return result;
        }
        HEAP_SUMMARY heap{};
        heap.cb = sizeof(heap);
        // HeapSummary does not promise to set last-error on every failure.
        ::SetLastError(ERROR_SUCCESS);
        result.bDefaultHeapAvailable = heapSummary(::GetProcessHeap(), 0, &heap) != FALSE;
        if (result.bDefaultHeapAvailable)
        {
            result.DefaultHeapAllocatedBytes = heap.cbAllocated;
            result.DefaultHeapCommittedBytes = heap.cbCommitted;
            result.DefaultHeapReservedBytes = heap.cbReserved;
        }
        else
        {
            const auto error = ::GetLastError();
            result.DefaultHeapError = error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE;
        }
        return result;
    }

    namespace
    {
        [[nodiscard]] std::wstring Utf8ToWide(std::string_view value)
        {
            if (value.empty())
                return {};
            const int required = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0);
            if (required <= 0)
                return std::wstring(value.begin(), value.end());

            std::wstring result(static_cast<std::size_t>(required), L'\0');
            if (::MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    required) <= 0)
            {
                return std::wstring(value.begin(), value.end());
            }
            return result;
        }

        [[nodiscard]] std::string WideToUtf8(std::wstring_view value)
        {
            if (value.empty())
                return {};
            const int required = ::WideCharToMultiByte(
                CP_UTF8,
                0,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (required <= 0)
				return "Windows error text conversion failed.";

            std::string result(static_cast<std::size_t>(required), '\0');
            if (::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    required,
                    nullptr,
                    nullptr) <= 0)
            {
				return "Windows error text conversion failed.";
            }
            return result;
        }

        [[nodiscard]] std::filesystem::path GetCurrentExecutablePath()
        {
            std::vector<wchar_t> buffer(512);
            while (buffer.size() <= 32768)
            {
                const DWORD length = ::GetModuleFileNameW(
                    nullptr,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()));
                if (length == 0)
                    throw std::runtime_error(
                        "GetModuleFileNameW failed with error " +
                        std::to_string(::GetLastError()));
                if (length < buffer.size())
                {
                    return std::filesystem::path(
                        buffer.data(),
                        buffer.data() + length);
                }
                buffer.resize(buffer.size() * 2);
            }
            throw std::runtime_error("The current executable path is too long.");
        }

        [[nodiscard]] std::wstring QuoteCommandLineArgument(
            std::wstring_view argument)
        {
            if (!argument.empty() &&
                argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
            {
                return std::wstring(argument);
            }

            std::wstring result(1, L'\"');
            std::size_t backslashCount = 0;
            for (const wchar_t character : argument)
            {
                if (character == L'\\')
                {
                    ++backslashCount;
                    continue;
                }
                if (character == L'\"')
                {
                    result.append(backslashCount * 2 + 1, L'\\');
                    result.push_back(character);
                    backslashCount = 0;
                    continue;
                }
                result.append(backslashCount, L'\\');
                backslashCount = 0;
                result.push_back(character);
            }
            result.append(backslashCount * 2, L'\\');
            result.push_back(L'\"');
            return result;
        }

        [[nodiscard]] std::string DescribeWindowsError(DWORD errorCode)
        {
            wchar_t* rawMessage = nullptr;
            const DWORD length = ::FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER |
                    FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                errorCode,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                reinterpret_cast<wchar_t*>(&rawMessage),
                0,
                nullptr);
            std::wstring message = length > 0 && rawMessage
                ? std::wstring(rawMessage, rawMessage + length)
                : L"Unknown Windows process error";
            if (rawMessage)
                ::LocalFree(rawMessage);
            while (!message.empty() &&
                (message.back() == L'\r' || message.back() == L'\n' ||
                    message.back() == L' '))
            {
                message.pop_back();
            }
            return WideToUtf8(message) + " (error " +
                std::to_string(errorCode) + ")";
        }
    }

    PlatformProcess::CapturedResult PlatformProcess::RunCaptured(
        const std::filesystem::path& executable, std::span<const std::string> arguments,
        std::stop_token stop, unsigned timeoutSeconds)
    {
        CapturedResult result;
        if (stop.stop_requested()) { result.Cancelled = true; result.Output = "Compiler cancelled before launch."; return result; }
        struct Close { void operator()(void* value) const { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); } };
        using Handle = std::unique_ptr<void, Close>;
        try
        {
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE readRaw = nullptr, writeRaw = nullptr;
            if (!::CreatePipe(&readRaw, &writeRaw, &security, 0)) throw std::runtime_error(DescribeWindowsError(::GetLastError()));
            Handle read(readRaw), write(writeRaw);
            ::SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0);
            Handle input(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (input.get() == INVALID_HANDLE_VALUE) throw std::runtime_error(DescribeWindowsError(::GetLastError()));
            Handle job(::CreateJobObjectW(nullptr, nullptr));
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job || !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                throw std::runtime_error(DescribeWindowsError(::GetLastError()));
            SIZE_T size = 0;
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            std::vector<std::byte> storage(size);
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &size)) throw std::runtime_error(DescribeWindowsError(::GetLastError()));
            struct AttributeCleanup { LPPROC_THREAD_ATTRIBUTE_LIST Value; ~AttributeCleanup() { ::DeleteProcThreadAttributeList(Value); } } cleanup{attributes};
            HANDLE inherited[]{write.get(), input.get()};
            if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
                throw std::runtime_error(DescribeWindowsError(::GetLastError()));
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write.get();
            startup.StartupInfo.hStdInput = input.get();
            startup.lpAttributeList = attributes;
            std::wstring command = QuoteCommandLineArgument(executable.wstring());
            for (const auto& argument : arguments) command += L" " + QuoteCommandLineArgument(Utf8ToWide(argument));
            PROCESS_INFORMATION processInfo{};
            if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                &startup.StartupInfo, &processInfo)) throw std::runtime_error(DescribeWindowsError(::GetLastError()));
            Handle process(processInfo.hProcess), thread(processInfo.hThread);
            struct ReapOnExit
            {
                HANDLE Process;
                HANDLE Job;
                ~ReapOnExit()
                {
                    // Also cover exceptions while allocating/copying captured output.
                    if (::WaitForSingleObject(Process, 0) != WAIT_OBJECT_0)
                    {
                        ::TerminateJobObject(Job, 1);
                        ::TerminateProcess(Process, 1);
                        ::WaitForSingleObject(Process, INFINITE);
                    }
                }
            } reap{process.get(), job.get()};
            if (!::AssignProcessToJobObject(job.get(), process.get()) || ::ResumeThread(thread.get()) == DWORD(-1))
            {
                ::TerminateProcess(process.get(), 1); ::WaitForSingleObject(process.get(), INFINITE);
                throw std::runtime_error("Could not attach the compiler to its owned process job.");
            }
            write.reset();
            const auto start = std::chrono::steady_clock::now();
            bool done = false;
            while (!done)
            {
                DWORD available = 0;
                std::size_t drained = 0;
                while (drained < 65536 && ::PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr) && available)
                {
                    char bytes[4096]; DWORD count = 0;
                    if (!::ReadFile(read.get(), bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr) || !count) break;
                    result.Output.append(bytes, count);
                    drained += count;
                    if (result.Output.size() > 1024 * 1024) result.Output.erase(0, result.Output.size() - 1024 * 1024);
                }
                done = ::WaitForSingleObject(process.get(), 20) == WAIT_OBJECT_0;
                if (!done && (stop.stop_requested() || std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSeconds)))
                {
                    result.Cancelled = true;
                    ::TerminateJobObject(job.get(), 1);
                    ::WaitForSingleObject(process.get(), INFINITE);
                    result.Output += "\nCompiler cancelled or timed out.\n";
                    done = true;
                }
            }
            // Drain final output after process exit; no descendant can keep the pipe alive.
            ::TerminateJobObject(job.get(), 0);
            DWORD available = 0;
            while (::PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr) && available)
            {
                char bytes[4096]; DWORD count = 0;
                if (!::ReadFile(read.get(), bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr) || !count) break;
                result.Output.append(bytes, count);
                if (result.Output.size() > 1024 * 1024) result.Output.erase(0, result.Output.size() - 1024 * 1024);
            }
            DWORD code = 1;
            ::GetExitCodeProcess(process.get(), &code);
            result.ExitCode = static_cast<int>(code);
        }
        catch (const std::exception& error) { result.Output += error.what(); }
        if (result.Output.size() > 1024 * 1024) result.Output.erase(0, result.Output.size() - 1024 * 1024);
        return result;
    }

    PlatformProcessLaunchResult PlatformProcess::RelaunchCurrentProcess(
        std::span<const std::string> arguments) noexcept
    {
        try
        {
            const std::filesystem::path executable = GetCurrentExecutablePath();
            std::wstring commandLine = QuoteCommandLineArgument(
                executable.wstring());
            for (const std::string& argument : arguments)
            {
                commandLine.push_back(L' ');
                commandLine += QuoteCommandLineArgument(Utf8ToWide(argument));
            }

            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};
            if (!::CreateProcessW(
                    executable.c_str(),
                    commandLine.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NEW_PROCESS_GROUP,
                    nullptr,
                    nullptr,
                    &startupInfo,
                    &processInfo))
            {
                return {
                    .Error = DescribeWindowsError(::GetLastError()),
                };
            }

            ::CloseHandle(processInfo.hThread);
            ::CloseHandle(processInfo.hProcess);
            return { .bSuccess = true };
        }
        catch (const std::exception& exception)
        {
            return { .Error = exception.what() };
        }
        catch (...)
        {
            return { .Error = "Unexpected process relaunch failure." };
        }
    }
}
