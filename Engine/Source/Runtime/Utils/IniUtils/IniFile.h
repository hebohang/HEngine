#pragma once

#include <filesystem>
#include <simpleIni/SimpleIni.h>

namespace HEngine
{
	class HIni
	{
	public:
		explicit HIni(const char* iniFilePath);

		const char* GetValue(const char* key, const char* section = "");
		void SetValue(const char* key, const char* newVal, const char* section = "");
	private:
		CSimpleIniA mIni;
	};
}
