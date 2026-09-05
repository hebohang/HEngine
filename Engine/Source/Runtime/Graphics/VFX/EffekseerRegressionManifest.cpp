#include "Hepch.h"

#include "Runtime/Graphics/VFX/EffekseerRegressionManifest.h"

#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace HEngine
{
    namespace
    {
        inline constexpr Utils::JsonLoadLimits ManifestLoadLimits{
            64u * 1024u,
            4,
            256
        };

        [[noreturn]] void FailManifest(
            const std::filesystem::path& manifestPath,
            std::string message)
        {
            throw std::runtime_error(
                "Effekseer regression manifest '" + Utils::PathToUtf8(manifestPath) +
                "': " + std::move(message));
        }

        [[nodiscard]] bool HasEffectPackageExtension(std::string_view normalizedPath)
        {
            const std::string cacheKey = Utils::MakeProjectPathCacheKey(normalizedPath);
            return cacheKey.size() >= 7 && cacheKey.ends_with(".efkpkg");
        }
    }

    EffekseerRegressionManifest LoadEffekseerRegressionManifest(
        const std::filesystem::path& projectRoot,
        std::string_view manifestProjectPath)
    {
        std::filesystem::path manifestPath;
        std::string normalizedManifestPath;
        const Utils::PathValidationResult manifestPathResult =
            Utils::TryResolveProjectRelativePath(
                projectRoot,
                manifestProjectPath,
                manifestPath,
                normalizedManifestPath);
        if (!manifestPathResult)
        {
            throw std::runtime_error(
                "Invalid Effekseer regression manifest path '" +
                std::string(manifestProjectPath) + "': " + manifestPathResult.Message);
        }

        std::error_code fileError;
        const bool bManifestExists = std::filesystem::is_regular_file(manifestPath, fileError);
        if (fileError && fileError != std::errc::no_such_file_or_directory)
            FailManifest(manifestPath, "cannot inspect the file: " + fileError.message());
        if (!bManifestExists)
            FailManifest(manifestPath, "file does not exist or is not a regular file.");

        Json root;
        try
        {
            root = Utils::LoadJsonFile(manifestPath, ManifestLoadLimits).Value;
        }
        catch (const std::exception& exception)
        {
            FailManifest(manifestPath, "cannot load JSON: " + std::string(exception.what()));
        }

        if (!root.is_object())
            FailManifest(manifestPath, "root must be an object.");

        const auto schemaVersion = root.find("SchemaVersion");
        if (schemaVersion == root.end() || !schemaVersion->is_number_integer())
            FailManifest(manifestPath, "SchemaVersion must be the integer 1.");
        if (schemaVersion->get<std::int64_t>() !=
            static_cast<std::int64_t>(EffekseerRegressionManifestLimits::SchemaVersion))
        {
            FailManifest(manifestPath, "SchemaVersion must be 1.");
        }

        const auto packages = root.find("Packages");
        if (packages == root.end() || !packages->is_array())
            FailManifest(manifestPath, "Packages must be an array of strings.");
        if (packages->size() > EffekseerRegressionManifestLimits::MaxPackages)
        {
            FailManifest(
                manifestPath,
                "Packages exceeds the maximum of " +
                    std::to_string(EffekseerRegressionManifestLimits::MaxPackages) + ".");
        }

        EffekseerRegressionManifest result;
        result.Packages.reserve(packages->size());
        std::unordered_set<std::string> packageKeys;
        packageKeys.reserve(packages->size());

        for (std::size_t index = 0; index < packages->size(); ++index)
        {
            const Json& package = (*packages)[index];
            const std::string fieldName = "Packages[" + std::to_string(index) + "]";
            if (!package.is_string())
                FailManifest(manifestPath, fieldName + " must be a string.");

            const std::string rawPath = package.get<std::string>();
            std::filesystem::path fullPath;
            std::string normalizedPath;
            const Utils::PathValidationResult packagePathResult =
                Utils::TryResolveProjectRelativePath(
                    projectRoot,
                    rawPath,
                    fullPath,
                    normalizedPath);
            if (!packagePathResult)
            {
                FailManifest(
                    manifestPath,
                    fieldName + " path '" + rawPath + "' is invalid: " +
                        packagePathResult.Message);
            }
            if (!HasEffectPackageExtension(normalizedPath))
            {
                FailManifest(
                    manifestPath,
                    fieldName + " must reference a project-relative .efkpkg file: '" +
                        normalizedPath + "'.");
            }

            const std::string packageKey = Utils::MakeProjectPathCacheKey(normalizedPath);
            if (!packageKeys.emplace(packageKey).second)
            {
                FailManifest(
                    manifestPath,
                    fieldName + " duplicates another package path case-insensitively: '" +
                        normalizedPath + "'.");
            }

            fileError.clear();
            const bool bPackageExists = std::filesystem::is_regular_file(fullPath, fileError);
            if (fileError && fileError != std::errc::no_such_file_or_directory)
            {
                FailManifest(
                    manifestPath,
                    fieldName + " cannot inspect package '" + normalizedPath + "': " +
                        fileError.message());
            }
            if (!bPackageExists)
            {
                FailManifest(
                    manifestPath,
                    fieldName + " package does not exist or is not a regular file: '" +
                        normalizedPath + "'.");
            }

            result.Packages.emplace_back(std::move(normalizedPath));
        }

        return result;
    }
}
