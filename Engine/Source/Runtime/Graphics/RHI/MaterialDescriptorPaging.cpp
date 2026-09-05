#include "Hepch.h"

#include "Runtime/Graphics/RHI/MaterialDescriptorPaging.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace HEngine
{
    MaterialDescriptorPagingResult BuildMaterialDescriptorPages(
        std::span<const MaterialDescriptorPageInput> materials,
        std::span<const MaterialDescriptorTextureKey> reservedTextureKeys,
        std::uint32_t pageCapacity)
    {
        if (pageCapacity == 0 || reservedTextureKeys.size() > pageCapacity)
        {
            throw std::invalid_argument(
                "Material descriptor page capacity cannot hold its reserved descriptors.");
        }

        std::unordered_set<MaterialDescriptorTextureKey> reservedUnique;
        for (const MaterialDescriptorTextureKey key : reservedTextureKeys)
        {
            if (key == 0 || !reservedUnique.insert(key).second)
            {
                throw std::invalid_argument(
                    "Material descriptor reserved texture keys must be nonzero and unique.");
            }
        }

        struct PageBuildState
        {
            std::vector<MaterialDescriptorTextureKey> Keys;
            std::unordered_map<MaterialDescriptorTextureKey, std::uint32_t>
                LocalIndices;
        };
        std::vector<PageBuildState> pages;
        MaterialDescriptorPagingResult result;
        result.Assignments.reserve(materials.size());

        const auto createPage = [&]() -> PageBuildState
        {
            PageBuildState page;
            page.Keys.reserve(pageCapacity);
            page.LocalIndices.reserve(pageCapacity);
            for (const MaterialDescriptorTextureKey key : reservedTextureKeys)
            {
                const std::uint32_t index =
                    static_cast<std::uint32_t>(page.Keys.size());
                page.Keys.push_back(key);
                page.LocalIndices.emplace(key, index);
            }
            return page;
        };

        for (const MaterialDescriptorPageInput& material : materials)
        {
            std::vector<MaterialDescriptorTextureKey> uniqueMaterialKeys;
            uniqueMaterialKeys.reserve(material.TextureKeys.size());
            std::unordered_set<MaterialDescriptorTextureKey> seen;
            for (const MaterialDescriptorTextureKey key : material.TextureKeys)
            {
                if (key == 0)
                {
                    throw std::invalid_argument(
                        "Material descriptor texture keys must be nonzero.");
                }
                if (seen.insert(key).second)
                    uniqueMaterialKeys.push_back(key);
            }

			std::size_t uniqueNonReservedCount = 0;
			for (const MaterialDescriptorTextureKey key : uniqueMaterialKeys)
				uniqueNonReservedCount += reservedUnique.contains(key) ? 0u : 1u;
			if (reservedTextureKeys.size() + uniqueNonReservedCount > pageCapacity)
            {
                throw std::length_error(
                    "Material " + std::to_string(material.MaterialIndex) +
                    " references more unique textures than one descriptor page can hold.");
            }

            std::uint32_t selectedPage =
                std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t pageIndex = 0;
                pageIndex < static_cast<std::uint32_t>(pages.size());
                ++pageIndex)
            {
                std::size_t additional = 0;
                for (const MaterialDescriptorTextureKey key : uniqueMaterialKeys)
                    additional += pages[pageIndex].LocalIndices.contains(key) ? 0u : 1u;
                if (pages[pageIndex].Keys.size() + additional <= pageCapacity)
                {
                    selectedPage = pageIndex;
                    break;
                }
            }
            if (selectedPage == std::numeric_limits<std::uint32_t>::max())
            {
                pages.push_back(createPage());
                selectedPage = static_cast<std::uint32_t>(pages.size() - 1u);
            }

            PageBuildState& page = pages[selectedPage];
            for (const MaterialDescriptorTextureKey key : uniqueMaterialKeys)
            {
                if (page.LocalIndices.contains(key))
                    continue;
                const std::uint32_t localIndex =
                    static_cast<std::uint32_t>(page.Keys.size());
                page.Keys.push_back(key);
                page.LocalIndices.emplace(key, localIndex);
            }

            MaterialDescriptorPageAssignment assignment{
                .MaterialIndex = material.MaterialIndex,
                .PageIndex = selectedPage,
            };
            assignment.LocalTextureIndices.reserve(material.TextureKeys.size());
            for (const MaterialDescriptorTextureKey key : material.TextureKeys)
                assignment.LocalTextureIndices.push_back(page.LocalIndices.at(key));
            result.Assignments.push_back(std::move(assignment));
        }

        result.Pages.reserve(pages.size());
        for (PageBuildState& page : pages)
        {
            result.PeakPageTextureCount = std::max(
                result.PeakPageTextureCount,
                static_cast<std::uint32_t>(page.Keys.size()));
            result.Pages.push_back(std::move(page.Keys));
        }
        return result;
    }
}
