#include "Hepch.h"
#include "Runtime/Utils/PlatformUtils.h"
#include "Runtime/Profile/CpuExecutionTimes.h"
#include "Runtime/Utils/StringUtils/StringUtils.h"
#include "Runtime/Platform/GLFW/CommonWindow.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <wrl/client.h>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "Runtime/Core/AppFramework/Application.h"

namespace HEngine 
{
    CpuExecutionTimes QueryCpuExecutionTimes() noexcept
    {
        FILETIME created{}, exited{}, threadKernel{}, threadUser{};
        FILETIME processKernel{}, processUser{};
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited,
                &threadKernel, &threadUser) ||
            !GetProcessTimes(GetCurrentProcess(), &created, &exited,
                &processKernel, &processUser))
            return {};
        const auto ticks = [](const FILETIME& value) noexcept -> uint64_t
        {
            return (uint64_t(value.dwHighDateTime) << 32u) | value.dwLowDateTime;
        };
        return { uint64_t(GetCurrentThreadId()),
            ticks(threadKernel) + ticks(threadUser),
            ticks(processKernel) + ticks(processUser), true };
    }

	namespace
	{
		[[nodiscard]] std::mutex& GetExternalApplicationStateMutex()
		{
			static std::mutex* mutex = new std::mutex();
			return *mutex;
		}

		[[nodiscard]] HWND GetDialogOwnerWindow()
		{
			auto* window = dynamic_cast<CommonWindow*>(&Application::GetWindow());
			return window ? glfwGetWin32Window(window->GetGlfwWindow()) : nullptr;
		}

        std::filesystem::path PickFolder(const std::string& initialDir, HWND owner)
        {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            if (FAILED(initialized)) return {};
            struct ApartmentScope
            {
                bool bUninitialize;
                ~ApartmentScope() { if (bUninitialize) CoUninitialize(); }
            } apartment{SUCCEEDED(initialized)};
            Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
            DWORD options = 0;
            if (FAILED(dialog->GetOptions(&options)) || FAILED(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR))) return {};
            dialog->SetTitle(L"选择目录 / Select Folder");
            if (!initialDir.empty())
            {
                auto initialPath = std::filesystem::absolute(std::filesystem::u8path(initialDir)).lexically_normal();
                std::error_code error;
                while (!initialPath.empty() && !std::filesystem::is_directory(initialPath, error))
                {
                    const auto parent = initialPath.parent_path();
                    if (parent == initialPath) break;
                    initialPath = parent;
                }
                Microsoft::WRL::ComPtr<IShellItem> folder;
                if (!initialPath.empty() && SUCCEEDED(SHCreateItemFromParsingName(initialPath.c_str(), nullptr, IID_PPV_ARGS(&folder))))
                    dialog->SetFolder(folder.Get());
            }
            const HRESULT shown = dialog->Show(owner);
            if (FAILED(shown))
            {
                if (shown != HRESULT_FROM_WIN32(ERROR_CANCELLED))
                    HE_CORE_WARN("Folder picker failed: HRESULT {}", static_cast<unsigned long>(shown));
                return {};
            }
            Microsoft::WRL::ComPtr<IShellItem> selected;
            if (FAILED(dialog->GetResult(&selected))) return {};
            PWSTR path = nullptr;
            if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
            const std::filesystem::path result(path);
            CoTaskMemFree(path);
            return result;
        }

        std::filesystem::path SelectFolderOnSta(const std::string& initialDir)
        {
            // Audio middleware may initialize the main thread as MTA. Shell
            // dialogs require STA; pump owner messages while the dialog is modal.
            const HWND owner = GetDialogOwnerWindow();
            auto result = std::async(std::launch::async, [initialDir, owner] { return PickFolder(initialDir, owner); });
            while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            return result.get();
        }

		[[nodiscard]] bool CopyStateFile(
			const std::filesystem::path& source,
			const std::filesystem::path& destination,
			bool bReportFailure = true)
		{
			std::error_code error;
			const bool bSourceExists = std::filesystem::exists(source, error);
			if (!error && !bSourceExists)
				error = std::make_error_code(std::errc::no_such_file_or_directory);
			if (!error)
			{
				const bool bRegularFile = std::filesystem::is_regular_file(source, error);
				if (!error && !bRegularFile)
					error = std::make_error_code(std::errc::invalid_argument);
			}
			if (!error)
				std::filesystem::create_directories(destination.parent_path(), error);
			if (!error)
			{
				std::filesystem::copy_file(
					source,
					destination,
					std::filesystem::copy_options::overwrite_existing,
					error);
			}
			if (error)
			{
				if (bReportFailure)
				{
					HE_CORE_WARN(
						"[ExternalApplication] Failed to copy state file '{0}' to '{1}': {2} ({3})",
						source.string(),
						destination.string(),
						error.message(),
						error.value());
				}
				return false;
			}
			return true;
		}

		[[nodiscard]] bool QueryOptionalStateFile(
			const std::filesystem::path& path,
			const char* role,
			bool& bExists)
		{
			std::error_code error;
			bExists = std::filesystem::exists(path, error);
			if (error)
			{
				HE_CORE_WARN(
					"[ExternalApplication] State cache could not inspect {0} '{1}': {2} ({3})",
					role,
					path.string(),
					error.message(),
					error.value());
				return false;
			}
			if (!bExists)
				return true;

			const bool bRegularFile = std::filesystem::is_regular_file(path, error);
			if (error || !bRegularFile)
			{
				HE_CORE_WARN(
					"[ExternalApplication] State cache {0} is not a regular file: {1}",
					role,
					path.string());
				return false;
			}
			return true;
		}

		[[nodiscard]] bool PrepareExternalApplicationState(
			std::span<const ExternalApplicationStateFile> stateFiles)
		{
			std::scoped_lock lock(GetExternalApplicationStateMutex());
			for (const ExternalApplicationStateFile& stateFile : stateFiles)
			{
				bool bCacheExists = false;
				if (!QueryOptionalStateFile(stateFile.CachePath, "cache file", bCacheExists))
					return false;

				// The runtime files are shared by every project. Never import a stale
				// runtime layout into another project's cache before launching.
				if (!bCacheExists)
				{
					if (!CopyStateFile(
							stateFile.DefaultPath,
							stateFile.CachePath))
					{
						return false;
					}
				}

				if (!CopyStateFile(
						stateFile.CachePath,
						stateFile.RuntimePath))
					return false;
			}
			return true;
		}

		void PersistExternalApplicationState(
			std::span<const ExternalApplicationStateFile> stateFiles) noexcept
		{
			try
			{
				std::scoped_lock lock(GetExternalApplicationStateMutex());
				for (const ExternalApplicationStateFile& stateFile : stateFiles)
					(void)CopyStateFile(
						stateFile.RuntimePath,
						stateFile.CachePath,
						false);
			}
			catch (...)
			{
			}
		}
	}
#if (!defined UNICODE) || (!defined _UNICODE)
	// https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-openfilenamea
	std::filesystem::path FileDialogs::OpenFile(const char* filter, std::string initialDir)
	{
		Utils::Replace(initialDir, '/', '\\');

		OPENFILENAME ofn;
		CHAR szFile[260] = { 0 };
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		ofn.lStructSize = sizeof(OPENFILENAME);
		ofn.hwndOwner = GetDialogOwnerWindow();
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = filter;
		ofn.lpstrInitialDir = initialDir.c_str();
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetOpenFileNameA(&ofn) == TRUE)
		{
			return ofn.lpstrFile;
		}
		return std::filesystem::path();
	}

	std::filesystem::path FileDialogs::SelectFolder(std::string initialDir)
	{
        return SelectFolderOnSta(initialDir);
	}

	std::filesystem::path FileDialogs::SaveFile(const char* filter, std::string initialDir)
	{
		std::string filterStr = filter;
		auto firstCharPos = filterStr.find_first_of('.');
		auto lastCharPos = filterStr.find_last_not_of(')');  // save file will only hace one extension
		std::string defExt = "";
		if (firstCharPos != std::string::npos && lastCharPos != std::string::npos)
		{
			defExt = filterStr.substr(firstCharPos + 1, lastCharPos - firstCharPos);
		}

		Utils::Replace(initialDir, '/', '\\');

		OPENFILENAME ofn;
		CHAR szFile[260] = { 0 };
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		ofn.lStructSize = sizeof(OPENFILENAME);
		ofn.hwndOwner = GetDialogOwnerWindow();
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = filter;
		ofn.nFilterIndex = 1;
		ofn.lpstrInitialDir = initialDir.c_str();
		ofn.lpstrDefExt = defExt.c_str();
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetSaveFileNameA(&ofn) == TRUE)
		{
			return ofn.lpstrFile;
		}
		return std::filesystem::path();
	}

	void FileDialogs::OpenInExplorer(std::string filePath)
	{
        const auto path = std::filesystem::u8path(filePath).make_preferred();
        std::error_code error;
		if (std::filesystem::is_directory(path, error))
		{
			::ShellExecuteW(nullptr, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
		else
		{
            const std::wstring arguments = L"/select,\"" + path.wstring() + L"\"";
			::ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
		}
	}
#else
	std::filesystem::path FileDialogs::OpenFile(const char* filter, std::string initialDir)
	{
		Utils::Replace(initialDir, '/', '\\');

		OPENFILENAME ofn;
		WCHAR szFile[260] = { 0 };
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		ofn.lStructSize = sizeof(OPENFILENAME);
		ofn.hwndOwner = GetDialogOwnerWindow();
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = Utils::AnsiToWString(filter).c_str();
		ofn.lpstrInitialDir = Utils::AnsiToWString(initialDir).c_str();
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetOpenFileNameW(&ofn) == TRUE)
		{
			return ofn.lpstrFile;
		}
		return std::filesystem::path();
	}

	std::filesystem::path FileDialogs::SaveFile(const char* filter, std::string initialDir)
	{
		std::string filterStr = filter;
		auto firstCharPos = filterStr.find_first_of('.');
		auto lastCharPos = filterStr.find_last_not_of(')');  // save file will only hace one extension
		std::string defExt = "";
		if (firstCharPos != std::string::npos && lastCharPos != std::string::npos)
		{
			defExt = filterStr.substr(firstCharPos + 1, lastCharPos - firstCharPos);
		}

		Utils::Replace(initialDir, '/', '\\');

		OPENFILENAME ofn;
		WCHAR szFile[260] = { 0 };
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		ofn.lStructSize = sizeof(OPENFILENAME);
		ofn.hwndOwner = GetDialogOwnerWindow();
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = Utils::AnsiToWString(filter).c_str();
		ofn.nFilterIndex = 1;
		ofn.lpstrInitialDir = Utils::AnsiToWString(initialDir).c_str();
		ofn.lpstrDefExt = Utils::AnsiToWString(defExt).c_str();
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetSaveFileNameW(&ofn) == TRUE)
		{
			return ofn.lpstrFile;
		}
		return std::filesystem::path();
	}

	std::filesystem::path FileDialogs::SelectFolder(std::string initialDir)
	{
        return SelectFolderOnSta(initialDir);
	}
#endif

    bool FileDialogs::OpenWebUrl(std::string_view url)
    {
        if (!url.starts_with("https://") && !url.starts_with("http://")) return false;
        const auto wideUrl = std::filesystem::u8path(url).wstring();
        return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }

	bool FileDialogs::OpenFileWithApplication(const std::filesystem::path& applicationPath,
	                                          const std::filesystem::path& filePath)
	{
		const std::wstring parameters = L"\"" + filePath.wstring() + L"\"";
		const std::wstring workingDirectory = applicationPath.parent_path().wstring();
		const HINSTANCE result = ::ShellExecuteW(
			nullptr,
			L"open",
			applicationPath.c_str(),
			parameters.c_str(),
			workingDirectory.c_str(),
			SW_SHOWNORMAL);
		return reinterpret_cast<INT_PTR>(result) > 32;
	}

	bool FileDialogs::OpenFileWithApplicationAndStateCache(
		const std::filesystem::path& applicationPath,
		const std::filesystem::path& filePath,
		std::span<const ExternalApplicationStateFile> stateFiles)
	{
		if (stateFiles.empty())
			return OpenFileWithApplication(applicationPath, filePath);

		bool bStateCacheReady = false;
		try
		{
			bStateCacheReady = PrepareExternalApplicationState(stateFiles);
			if (!bStateCacheReady)
			{
				HE_CORE_WARN(
					"[ExternalApplication] State cache preparation failed; launching without state persistence. application='{0}'",
					applicationPath.string());
			}
		}
		catch (const std::exception& exception)
		{
			HE_CORE_WARN(
				"[ExternalApplication] State cache preparation threw an exception; launching without state persistence. application='{0}', error='{1}'",
				applicationPath.string(),
				exception.what());
		}
		catch (...)
		{
			HE_CORE_WARN(
				"[ExternalApplication] State cache preparation threw an unknown exception; launching without state persistence. application='{0}'",
				applicationPath.string());
		}

		const std::wstring parameters = L"\"" + filePath.wstring() + L"\"";
		const std::wstring workingDirectory = applicationPath.parent_path().wstring();
		SHELLEXECUTEINFOW executeInfo{};
		executeInfo.cbSize = sizeof(executeInfo);
		executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
		executeInfo.hwnd = GetDialogOwnerWindow();
		executeInfo.lpVerb = L"open";
		executeInfo.lpFile = applicationPath.c_str();
		executeInfo.lpParameters = parameters.c_str();
		executeInfo.lpDirectory = workingDirectory.c_str();
		executeInfo.nShow = SW_SHOWNORMAL;
		::SetLastError(ERROR_SUCCESS);
		if (::ShellExecuteExW(&executeInfo) == FALSE)
		{
			const DWORD errorCode = ::GetLastError();
			const std::error_code error(
				static_cast<int>(errorCode),
				std::system_category());
			HE_CORE_ERROR(
				"[ExternalApplication] ShellExecuteExW failed. application='{0}', file='{1}', workingDirectory='{2}', error='{3}' ({4})",
				applicationPath.string(),
				filePath.string(),
				applicationPath.parent_path().string(),
				error.message(),
				errorCode);
			return false;
		}
		if (executeInfo.hProcess == nullptr)
		{
			HE_CORE_WARN(
				"[ExternalApplication] Application launched without a process handle; state persistence is unavailable. application='{0}', file='{1}'",
				applicationPath.string(),
				filePath.string());
			return true;
		}

		try
		{
			std::vector<ExternalApplicationStateFile> ownedStateFiles;
			if (bStateCacheReady)
			{
				ownedStateFiles.assign(stateFiles.begin(), stateFiles.end());
			}
			const DWORD processId = ::GetProcessId(executeInfo.hProcess);
			std::thread([
				process = executeInfo.hProcess,
				stateFiles = std::move(ownedStateFiles)]() noexcept
			{
				const DWORD waitResult = ::WaitForSingleObject(process, INFINITE);
				::CloseHandle(process);
				if (waitResult == WAIT_OBJECT_0)
				{
					if (!stateFiles.empty())
						PersistExternalApplicationState(stateFiles);
				}
			}).detach();
			HE_CORE_INFO(
				"[ExternalApplication] Launched application. application='{0}', file='{1}', workingDirectory='{2}', processId={3}, stateCache={4}",
				applicationPath.string(),
				filePath.string(),
				applicationPath.parent_path().string(),
				processId,
				bStateCacheReady ? "ready" : "disabled");
		}
		catch (const std::exception& exception)
		{
			HE_CORE_WARN(
				"[ExternalApplication] Application launched, but process monitoring could not start: {0}",
				exception.what());
			::CloseHandle(executeInfo.hProcess);
		}
		catch (...)
		{
			HE_CORE_WARN(
				"[ExternalApplication] Application launched, but process monitoring failed with an unknown exception.");
			::CloseHandle(executeInfo.hProcess);
		}
		return true;
	}
}
