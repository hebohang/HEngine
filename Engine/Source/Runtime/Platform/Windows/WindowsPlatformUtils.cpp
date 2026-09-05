#include "Hepch.h"
#include "Runtime/Utils/PlatformUtils.h"
#include "Runtime/Utils/StringUtils/StringUtils.h"
#include "Runtime/Platform/GLFW/CommonWindow.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
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
		Utils::Replace(initialDir, '/', '\\');

		OPENFILENAME ofn;
		CHAR szFile[260] = { 0 };
		strcpy_s(szFile, "SelectFolder");
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		ofn.lStructSize = sizeof(OPENFILENAME);
		ofn.hwndOwner = GetDialogOwnerWindow();
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = "All Files\0*.*\0\0";
		ofn.lpstrInitialDir = initialDir.c_str();
		ofn.lpstrTitle = "Select Folder";
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_NOVALIDATE;
		if (GetSaveFileNameA(&ofn) == TRUE)
		{
			std::filesystem::path result(ofn.lpstrFile);
			return result.parent_path();
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
		Utils::Replace(filePath, '/', '\\');
		if (std::filesystem::is_directory(filePath))
		{
			::ShellExecuteA(NULL, "explore", filePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
		}
		else
		{
			filePath = "/select," + filePath;
			::ShellExecuteA(NULL, "open", "explorer.exe", filePath.c_str(), NULL, SW_SHOWNORMAL);
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
		Utils::Replace(initialDir, '/', '\\');

		std::wstring wInitialDir = Utils::AnsiToWString(initialDir);
		std::wstring wTitle = L"Select Folder";
		std::wstring wFilter = L"All Files\0*.*\0\0";

		OPENFILENAME ofn;
		WCHAR szFile[260] = { 0 };
		wcscpy_s(szFile, L"SelectFolder");
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		ofn.lStructSize = sizeof(OPENFILENAME);
		ofn.hwndOwner = GetDialogOwnerWindow();
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile) / sizeof(WCHAR);
		ofn.lpstrFilter = wFilter.c_str();
		ofn.lpstrInitialDir = wInitialDir.c_str();
		ofn.lpstrTitle = wTitle.c_str();
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_NOVALIDATE;
		if (GetSaveFileNameW(&ofn) == TRUE)
		{
			std::filesystem::path result(ofn.lpstrFile);
			return result.parent_path();
		}
		return std::filesystem::path();
	}
#endif

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
