#include "Hepch.h"

#include "Runtime/Platform/PlatformPaths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <stdexcept>
#include <vector>

namespace HEngine
{
    namespace
    {
        static std::wstring Utf8ToWide(std::string_view value)
        {
            if (value.empty())
                return {};

            const int requiredSize = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0);
            if (requiredSize <= 0)
                return std::wstring(value.begin(), value.end());

            std::wstring result(static_cast<std::size_t>(requiredSize), L'\0');
            if (::MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    requiredSize) <= 0)
            {
                return std::wstring(value.begin(), value.end());
            }
            return result;
        }
    }

    std::filesystem::path PlatformPaths::GetExecutableDirectory()
    {
        std::vector<wchar_t> buffer(512);
        while (buffer.size() <= 32768)
        {
            ::SetLastError(ERROR_SUCCESS);
            const DWORD length = ::GetModuleFileNameW(
                nullptr,
                buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (length == 0)
                throw std::runtime_error("Could not locate the game executable.");

            if (length < buffer.size())
                return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();

            buffer.resize(buffer.size() * 2);
        }

        throw std::runtime_error("The game executable path is too long.");
    }

    std::filesystem::path PlatformPaths::GetLocalAppDataDirectory()
    {
        PWSTR rawPath = nullptr;
        const HRESULT result = ::SHGetKnownFolderPath(
            FOLDERID_LocalAppData,
            KF_FLAG_DEFAULT,
            nullptr,
            &rawPath);
        if (FAILED(result) || rawPath == nullptr || rawPath[0] == L'\0')
        {
            if (rawPath != nullptr)
                ::CoTaskMemFree(rawPath);
            throw std::runtime_error("Could not locate the current user's Local AppData folder.");
        }

        const std::filesystem::path localAppData(rawPath);
        ::CoTaskMemFree(rawPath);
        return localAppData;
    }

    std::filesystem::path PlatformPaths::GetPowerShellExecutable()
    {
        std::vector<wchar_t> buffer(32768);
        const UINT length = ::GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (!length || length >= buffer.size())
            throw std::runtime_error("Could not locate Windows PowerShell.");
        return std::filesystem::path(buffer.data(), buffer.data() + length) / "WindowsPowerShell/v1.0/powershell.exe";
    }

    std::filesystem::path PlatformPaths::ToFileIoPath(const std::filesystem::path& path)
    {
        // CRT streams do not automatically opt into long paths. Normalize before
        // applying the extended prefix, whose Win32 semantics disable dot parsing.
        const std::wstring nativePath = std::filesystem::absolute(path).lexically_normal().make_preferred().native();
        if (nativePath.starts_with(L"\\\\?\\"))
            return nativePath;
        if (nativePath.starts_with(L"\\\\"))
            return L"\\\\?\\UNC\\" + nativePath.substr(2);
        return L"\\\\?\\" + nativePath;
    }

    void PlatformPaths::ShowFatalDialog(std::string_view title, std::string_view message) noexcept
    {
        try
        {
            const std::wstring wideTitle = Utf8ToWide(title);
            const std::wstring wideMessage = Utf8ToWide(message);
            ::MessageBoxW(
                nullptr,
                wideMessage.c_str(),
                wideTitle.c_str(),
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
        }
        catch (...)
        {
            ::MessageBoxW(
                nullptr,
                L"Starbloom Survivor could not start.",
                L"Starbloom Survivor",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
        }
    }
}
