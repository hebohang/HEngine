#include "Hepch.h"

#include "Runtime/Platform/PlatformProcess.h"

#include <Windows.h>

#include <filesystem>
#include <string_view>
#include <vector>

namespace HEngine
{
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
