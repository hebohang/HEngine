#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ResType/Material.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <exception>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace HEngine
{
    class AssetManager : public PublicSingleton<AssetManager>
    {
    public:
        // Returns false after logging any serialization or atomic-save failure.
        // An existing destination is preserved on failure.
        template<typename AssetType>
        static bool SaveAsset(const AssetType& asset, const std::filesystem::path& outPath)
        {
            return SaveAsset(asset, outPath, Utils::AtomicWriteMode::ReplaceExisting);
        }

        template<typename AssetType>
        static bool SaveAsset(
            const AssetType& asset,
            const std::filesystem::path& outPath,
            Utils::AtomicWriteMode writeMode)
        {
            try
            {
                Json json;
                Serializer::Serialize(json, asset);
                Utils::SaveJsonToFile(json, outPath, writeMode);
                return true;
            }
            catch (const std::exception& exception)
            {
                HE_CORE_ERROR("Save asset {0} failed: {1}", outPath.string(), exception.what());
                return false;
            }
            catch (...)
            {
                HE_CORE_ERROR("Save asset {0} failed with an unknown error.", outPath.string());
                return false;
            }
        }

        template<typename AssetType>
        static bool SaveAsset(const AssetType& asset, const std::string& outPath)
        {
            return AssetManager::SaveAsset(asset, ConfigManager::GetProjectFullPath(outPath));
        }

        template<typename ResType>
        static std::filesystem::path NewAsset(const ResType& asset, const char* defaultName, const std::filesystem::path& outDir)
        {
            std::string extensionName = ResType::GetExtName();
            auto fileName = std::string(defaultName) + std::string(".") + extensionName;
            auto newAssetFile = outDir / std::filesystem::path(fileName);
            int index = 1;
            while (std::filesystem::exists(newAssetFile))
            {
                std::string newFileName = std::string(defaultName) + std::to_string(index) + std::string(".") + extensionName;
                newAssetFile = outDir / std::filesystem::path(newFileName);
                index++;
            }
            return AssetManager::SaveAsset(asset, newAssetFile, Utils::AtomicWriteMode::CreateNew)
                ? newAssetFile
                : std::filesystem::path();
        }

        template<typename ResType>
        static std::filesystem::path NewAsset(const char* defaultName, const std::filesystem::path& outDir)
        {
            ResType asset;
            return AssetManager::NewAsset(asset, defaultName, outDir);
        }

        template<>
        std::filesystem::path NewAsset<MaterialRes>(const char* defaultName, const std::filesystem::path& outDir);

        // if a.b exsist, will get a_0.b (a_1.b, a_2.b, ...)
        static std::filesystem::path GetAvailableFileName(const char* defaultNameWithExt, const std::filesystem::path& outDir);

        // Selects one stem for a group such as { ".bin", ".hmesh" }.
        static std::filesystem::path GetAvailableFileStem(
            std::string_view defaultStem,
            const std::filesystem::path& outDir,
            std::initializer_list<std::string_view> extensions);
    };
}
