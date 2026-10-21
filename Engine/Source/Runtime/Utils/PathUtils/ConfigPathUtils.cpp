#include "Hepch.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

namespace HEngine::Utils
{
    namespace
    {
        std::string ToSlash(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            return value;
        }
    }
    std::string NormalizeAssetPath(const std::string& path)
    {
        if (path.empty())
            return std::string();

        const std::string slashPath = ToSlash(path);
        std::filesystem::path fsPath = Utf8ToPath(slashPath);
        fsPath = fsPath.lexically_normal();

        if (fsPath.is_absolute())
        {
            const std::filesystem::path root = ConfigManager::GetInstance().GetRootFolder();
            std::error_code ec;
            const std::filesystem::path relative = std::filesystem::relative(fsPath, root, ec);
            if (!ec && !relative.empty() && PathToUtf8(relative).rfind("..", 0) != 0)
                fsPath = relative;
        }

        std::string normalized = ToSlash(PathToUtf8(fsPath));

        while (normalized.find("//") != std::string::npos)
            normalized = ToSlash(PathToUtf8(Utf8ToPath(normalized).lexically_normal()));

        if (normalized == ".")
            normalized.clear();

        return normalized;
    }

    bool IsSameAssetPath(const std::string& lhs, const std::string& rhs)
    {
        return NormalizeAssetPath(lhs) == NormalizeAssetPath(rhs);
    }
}
