#pragma once
#include "Runtime/Core/Base/Base.h"

#ifdef HE_PLATFORM_WINDOWS

#include <string>
#include <Windows.h>
#include <winreg.h>

namespace HEngine
{
	namespace Utils
	{
		bool QueryRegKey(const ::HKEY hKey, const char* subKey, const char* valueName, std::string& outValue)
		{
			HKEY Key = 0;

			// 考虑32位和64位两种情况
			for (int i = 0; i < 2; i++)
			{
				auto flag = (i == 0) ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
				if (RegOpenKeyEx(hKey, subKey, 0, KEY_READ | flag, &Key) == ERROR_SUCCESS)
				{
					DWORD dataSize{};

					// refer to https://learn.microsoft.com/zh-cn/archive/msdn-magazine/2017/may/c-use-modern-c-to-access-the-windows-registry
					LONG retCode = ::RegGetValue(
						hKey,
						subKey,
						valueName,
						RRF_RT_REG_SZ,
						nullptr,
						nullptr,
						&dataSize
					);
					if (retCode != ERROR_SUCCESS)
					{
						HE_CORE_ERROR("Can not find the key in regex: {0}, {1}", subKey, valueName);
						return false;
					}

					char* Buffer = new char[dataSize];
					if (RegQueryValueEx(Key, valueName, NULL, NULL, (LPBYTE)Buffer, &dataSize) == ERROR_SUCCESS)
					{
						outValue = std::string(Buffer);
						delete[] Buffer;
						return true;
					}
					else
					{
						HE_CORE_ERROR("Can not find the key in regex: {0}, {1}", subKey, valueName);
						delete[] Buffer;
						return false;
					}
				}
			}

			HE_CORE_ERROR("Can not find the key in regex: {0}, {1}", subKey, valueName);
			return false;
		}
	}
}

#endif