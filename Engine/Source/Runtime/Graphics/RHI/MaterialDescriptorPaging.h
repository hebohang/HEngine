#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace HEngine
{
    using MaterialDescriptorTextureKey = std::uint64_t;

    struct MaterialDescriptorPageInput
    {
        std::uint32_t MaterialIndex = 0;
        std::vector<MaterialDescriptorTextureKey> TextureKeys;
    };

    struct MaterialDescriptorPageAssignment
    {
        std::uint32_t MaterialIndex = 0;
        std::uint32_t PageIndex = 0;
        std::vector<std::uint32_t> LocalTextureIndices;
    };

    struct MaterialDescriptorPagingResult
    {
        std::vector<std::vector<MaterialDescriptorTextureKey>> Pages;
        std::vector<MaterialDescriptorPageAssignment> Assignments;
        std::uint32_t PeakPageTextureCount = 0;
    };

    // Stable first-fit paging. Every page begins with the same reserved
    // descriptors, and a material is never split across pages.
    [[nodiscard]] MaterialDescriptorPagingResult BuildMaterialDescriptorPages(
        std::span<const MaterialDescriptorPageInput> materials,
        std::span<const MaterialDescriptorTextureKey> reservedTextureKeys,
        std::uint32_t pageCapacity);
}
