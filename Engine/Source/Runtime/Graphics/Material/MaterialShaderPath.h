#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace HEngine
{
    // Material shader identities are content-relative and use one canonical
    // spelling so they cannot address arbitrary generated artifacts.
    [[nodiscard]] bool TryNormalizeMaterialShaderPath(
        std::string_view shaderPath,
        std::string& normalizedPath,
        std::string& error);

    // Resolves the published manifest and verifies both the generated
    // Materials root and the final path after following existing reparse
    // points (symbolic links/junctions).
    [[nodiscard]] bool TryResolvePublishedMaterialShaderManifestPath(
        const std::filesystem::path& engineGeneratedContentRoot,
        std::string_view shaderPath,
        std::filesystem::path& manifestPath,
        std::string& normalizedShaderPath,
        std::string& error);
}
