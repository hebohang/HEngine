#include "Hepch.h"
#include "StringUtils.h"

#include <locale>
#include <codecvt>

namespace HEngine::Utils
{
    std::string Replace(std::string& source_string, std::string sub_string, const std::string new_string)
    {
        std::string::size_type pos = 0;
        while ((pos = source_string.find(sub_string)) != std::string::npos)
        {
            source_string.replace(pos, sub_string.length(), new_string);
        }
        return source_string;
    }

    std::string Replace(std::string& source_string, char taget_char, const char new_char)
    {
        std::replace(source_string.begin(), source_string.end(), taget_char, new_char);
        return source_string;
    }

    std::string ToUpper(std::string& source_string)
    {
        transform(source_string.begin(), source_string.end(), source_string.begin(), ::toupper);
        return source_string;
    }

    std::wstring AnsiToWString(const std::string& str)
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        return converter.from_bytes(str);
    }

    std::string WStringToAnsi(const std::wstring& wstr)
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        return converter.to_bytes(wstr);
    }
}
