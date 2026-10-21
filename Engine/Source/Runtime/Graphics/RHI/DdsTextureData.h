#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace HEngine
{
    enum class DdsBlockFormat : uint8_t { BC1, BC3, BC5, BC7, RGBA16Float };

    struct DdsMipData
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        size_t Offset = 0;
        size_t RowPitch = 0;
        size_t SlicePitch = 0;
    };

    // DDS is an asset container, independent of a graphics API. Only immutable
    // 2D UNORM block-compressed or linear RGBA16F textures are accepted;
    // every mip is file-backed, including the 1x1 HDR tail.
    struct DdsTextureData
    {
        DdsBlockFormat Format = DdsBlockFormat::BC1;
        bool bSRgb = false;
        std::vector<DdsMipData> Mips;
        std::vector<std::byte> Bytes;
        uint32_t SourceWidth = 0, SourceHeight = 0, FirstResidentMip = 0;
        // Bytes requested from the file by LoadMipTail; zero for staged data.
        size_t FileBytesRead = 0;

        [[nodiscard]] bool HasCompleteMipChain() const noexcept;
        [[nodiscard]] static DdsTextureData Parse(std::span<const std::byte> bytes);
        [[nodiscard]] static DdsTextureData Load(const std::filesystem::path& path);
        // Size limit selects the logical source mip before firstResidentMip.
        // Returns only selected mips, with offsets relative to the owned tail.
        [[nodiscard]] static DdsTextureData LoadMipTail(const std::filesystem::path& path,
            uint32_t maximumDimension, uint32_t firstResidentMip);
    };
}
