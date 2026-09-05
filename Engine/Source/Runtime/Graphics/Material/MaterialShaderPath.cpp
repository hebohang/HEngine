#include "Hepch.h"

#include "Runtime/Graphics/Material/MaterialShaderPath.h"

#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] std::string ToLowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        [[nodiscard]] bool IsSamePathComponent(
            const std::filesystem::path& lhs,
            const std::filesystem::path& rhs)
        {
#if defined(_WIN32)
            return ToLowerAscii(Utils::PathToUtf8(lhs)) ==
                ToLowerAscii(Utils::PathToUtf8(rhs));
#else
            return lhs == rhs;
#endif
        }

        [[nodiscard]] bool IsPhysicallyWithin(
            const std::filesystem::path& root,
            const std::filesystem::path& candidate)
        {
            auto rootIterator = root.begin();
            auto candidateIterator = candidate.begin();
            for (; rootIterator != root.end(); ++rootIterator, ++candidateIterator)
            {
                if (candidateIterator == candidate.end() ||
                    !IsSamePathComponent(*rootIterator, *candidateIterator))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool ResolvePhysicalPath(
            const std::filesystem::path& path,
            std::filesystem::path& resolved,
            std::string& error)
        {
            std::error_code filesystemError;
            const std::filesystem::path absolutePath =
                std::filesystem::absolute(path, filesystemError);
            if (filesystemError)
            {
                error = "Could not make generated material path absolute: " +
                    filesystemError.message();
                return false;
            }
            resolved = std::filesystem::weakly_canonical(absolutePath, filesystemError);
            if (filesystemError)
            {
                error = "Could not resolve generated material path: " +
                    filesystemError.message();
                return false;
            }
            return true;
        }
    }

    bool TryNormalizeMaterialShaderPath(
        std::string_view shaderPath,
        std::string& normalizedPath,
        std::string& error)
    {
        normalizedPath.clear();
        error.clear();

        const Utils::PathValidationResult validation =
            Utils::TryNormalizeProjectRelativePath(shaderPath, normalizedPath);
        if (!validation)
        {
            error = validation.Message;
            return false;
        }

        constexpr std::string_view shaderPrefix = "Shaders/";
        if (!normalizedPath.starts_with(shaderPrefix) ||
            ToLowerAscii(Utils::PathToUtf8(
                Utils::Utf8ToPath(normalizedPath).extension())) != ".hlsl")
        {
            error = "Material shader path must be a canonical Shaders-relative .hlsl path.";
            normalizedPath.clear();
            return false;
        }
        return true;
    }

    bool TryResolvePublishedMaterialShaderManifestPath(
        const std::filesystem::path& engineGeneratedContentRoot,
        std::string_view shaderPath,
        std::filesystem::path& manifestPath,
        std::string& normalizedShaderPath,
        std::string& error)
    {
        manifestPath.clear();
        if (!TryNormalizeMaterialShaderPath(shaderPath, normalizedShaderPath, error))
            return false;

        std::filesystem::path physicalGeneratedRoot;
        if (!ResolvePhysicalPath(engineGeneratedContentRoot, physicalGeneratedRoot, error))
            return false;

        const std::filesystem::path logicalMaterialsRoot =
            engineGeneratedContentRoot / "Shaders" / "CACHES" / "Materials";
        std::filesystem::path physicalMaterialsRoot;
        if (!ResolvePhysicalPath(logicalMaterialsRoot, physicalMaterialsRoot, error))
            return false;
        if (!IsPhysicallyWithin(physicalGeneratedRoot, physicalMaterialsRoot))
        {
            error = "Generated Materials root resolves outside engine generated content.";
            return false;
        }

        constexpr std::string_view shaderPrefix = "Shaders/";
        std::filesystem::path logicalManifestPath = logicalMaterialsRoot /
            Utils::Utf8ToPath(normalizedShaderPath.substr(shaderPrefix.size()));
        logicalManifestPath += ".material.json";

        std::filesystem::path physicalManifestPath;
        if (!ResolvePhysicalPath(logicalManifestPath, physicalManifestPath, error))
            return false;
        if (!IsPhysicallyWithin(physicalMaterialsRoot, physicalManifestPath))
        {
            error = "Published material manifest resolves outside the generated Materials root.";
            return false;
        }

        manifestPath = std::move(physicalManifestPath);
        return true;
    }
}
