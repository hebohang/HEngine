#include "Hepch.h"
#include "AssetManager.h"
#include "Runtime/Resource/ResType/BasePbrMaterialBuilder.h"
#include "Runtime/Resource/ResType/Material.h"
#include "_generated/serializer/Material.gen.h"

#include <stdexcept>

namespace HEngine
{
    std::filesystem::path AssetManager::GetAvailableFileStem(
        std::string_view defaultStem,
        const std::filesystem::path& outDir,
        std::initializer_list<std::string_view> extensions)
    {
        if (defaultStem.empty())
            throw std::invalid_argument("Asset file stem cannot be empty.");
        if (extensions.size() == 0)
            throw std::invalid_argument("At least one asset extension is required.");

        const std::string baseStem(defaultStem);
        std::string candidateStem = baseStem;
        int index = 0;

        const auto bCandidateExists = [&outDir, extensions](const std::string& stem)
        {
            for (std::string_view extension : extensions)
            {
                std::string normalizedExtension(extension);
                if (!normalizedExtension.empty() && normalizedExtension.front() != '.')
                    normalizedExtension.insert(normalizedExtension.begin(), '.');

                std::filesystem::path candidatePath = outDir / stem;
                candidatePath += normalizedExtension;
                if (std::filesystem::exists(candidatePath))
                    return true;
            }
            return false;
        };

        while (bCandidateExists(candidateStem))
            candidateStem = baseStem + '_' + std::to_string(index++);

        return outDir / candidateStem;
    }

    std::filesystem::path AssetManager::GetAvailableFileName(const char* defaultNameWithExt, const std::filesystem::path& outDir)
    {
        auto defaultNameWithExtP = std::filesystem::path(defaultNameWithExt);
        auto newPath = outDir / defaultNameWithExtP;
        int index = 0;
        while (std::filesystem::exists(newPath))
        {
            std::string newName = defaultNameWithExtP.stem().string() + '_' + std::to_string(index) + defaultNameWithExtP.extension().string();
            newPath = outDir / std::filesystem::path(newName);
            index++;
        }
        return newPath;
    }

    template<>
    std::filesystem::path AssetManager::NewAsset<MaterialRes>(const char* defaultName, const std::filesystem::path& outDir)
    {
        const MaterialRes matRes = BuildBasePbrMaterialResV2(PbrMaterialRes{});
        return AssetManager::NewAsset(matRes, defaultName, outDir);
    }
}
