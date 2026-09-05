#pragma once

#include <string>

namespace HEngine
{
	namespace Utils
	{
		std::string Replace(std::string& source_string, std::string sub_string, const std::string new_string);

		std::string Replace(std::string& source_string, char taget_char, const char new_char);

		std::string ToUpper(std::string& source_string);

		std::wstring AnsiToWString(const std::string& str);
		std::string WStringToAnsi(const std::wstring& wstr);
	}
}
