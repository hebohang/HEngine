#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace HEngine::EffekseerPackage
{
    // Effekseer paths are UTF-16 on every platform. wchar_t is UTF-32 on macOS
    // and cannot be used as an interchangeable string representation.
    [[nodiscard]] inline std::string Utf16PathToUtf8(std::u16string_view path)
    {
        const auto utf8 = std::filesystem::path(std::u16string(path)).generic_u8string();
        return { utf8.begin(), utf8.end() };
    }

    [[nodiscard]] inline std::u16string Utf8PathToUtf16(std::string_view path)
    {
        return std::filesystem::path(std::u8string(path.begin(), path.end())).generic_u16string();
    }

    [[nodiscard]] std::filesystem::path ExtractForPreview(const std::string& relativePath);
}
