#pragma once

#include <filesystem>
#include <string>

namespace HEngine::EffekseerPackage
{
    [[nodiscard]] std::filesystem::path ExtractForPreview(const std::string& relativePath);
}
