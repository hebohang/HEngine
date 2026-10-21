#include "Hepch.h"

#include "DdsTextureData.h"
#include "Runtime/Resource/AssetReadAhead.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr size_t c_MaxDdsBytes = 512u * 1024u * 1024u;

        uint32_t ReadU32(std::span<const std::byte> bytes, size_t offset)
        {
            if (offset > bytes.size() || bytes.size() - offset < 4)
                throw std::runtime_error("Truncated DDS header");
            uint32_t value = 0;
            for (uint32_t index = 0; index < 4; ++index)
                value |= std::to_integer<uint32_t>(bytes[offset + index]) << (index * 8u);
            return value;
        }

        constexpr uint32_t FourCC(char a, char b, char c, char d)
        {
            return uint32_t(a) | uint32_t(b) << 8u | uint32_t(c) << 16u | uint32_t(d) << 24u;
        }
    }

    bool DdsTextureData::HasCompleteMipChain() const noexcept
    {
        return !Mips.empty() && Mips.back().Width == 1 && Mips.back().Height == 1;
    }

    static DdsTextureData ParseDdsMetadata(std::span<const std::byte> bytes, size_t fileSize)
    {
        if (bytes.size() < 128 || fileSize < 128 || fileSize > c_MaxDdsBytes ||
            ReadU32(bytes, 0) != FourCC('D', 'D', 'S', ' ') ||
            ReadU32(bytes, 4) != 124 || ReadU32(bytes, 76) != 32)
            throw std::runtime_error("Invalid DDS magic, header size or file size");

        const uint32_t flags = ReadU32(bytes, 8);
        uint32_t height = ReadU32(bytes, 12);
        uint32_t width = ReadU32(bytes, 16);
        const uint32_t depth = ReadU32(bytes, 24);
        const uint32_t caps2 = ReadU32(bytes, 112);
        if ((flags & 0x1007u) != 0x1007u || (ReadU32(bytes, 108) & 0x1000u) == 0 ||
            width == 0 || height == 0 || width > 16384 || height > 16384)
            throw std::runtime_error("Invalid DDS 2D dimensions or required flags");
        if (depth > 1 || (flags & 0x800000u) != 0 || (caps2 & 0x20fe00u) != 0)
            throw std::runtime_error("DDS volume and cubemap textures are unsupported");
        if ((ReadU32(bytes, 80) & 4u) == 0)
            throw std::runtime_error("DDS requires a supported block-compressed FourCC");

        DdsTextureData result;
        result.SourceWidth = width;
        result.SourceHeight = height;
        size_t offset = 128;
        const uint32_t fourCC = ReadU32(bytes, 84);
        if (fourCC == FourCC('D', 'X', 'T', '1')) result.Format = DdsBlockFormat::BC1;
        else if (fourCC == FourCC('D', 'X', 'T', '5')) result.Format = DdsBlockFormat::BC3;
        else if (fourCC == FourCC('A', 'T', 'I', '2') || fourCC == FourCC('B', 'C', '5', 'U'))
            result.Format = DdsBlockFormat::BC5;
        else if (fourCC == FourCC('D', 'X', '1', '0'))
        {
            // Numeric values are the published DDS extension's file encoding.
            // No native graphics header or API type is needed to decode them.
            if (ReadU32(bytes, 132) != 3 || ReadU32(bytes, 140) != 1 ||
                (ReadU32(bytes, 136) & 4u) != 0)
                throw std::runtime_error("DDS requires a single 2D texture, without cubemap faces");
            const uint32_t alphaMode = ReadU32(bytes, 144) & 7u;
            if (alphaMode == 2)
                throw std::runtime_error("Premultiplied-alpha DDS is incompatible with straight-alpha materials");
            const uint32_t format = ReadU32(bytes, 128);
            switch (format)
            {
            case 10: result.Format = DdsBlockFormat::RGBA16Float; break;
            case 71: case 72: result.Format = DdsBlockFormat::BC1; break;
            case 77: case 78: result.Format = DdsBlockFormat::BC3; break;
            case 83: result.Format = DdsBlockFormat::BC5; break;
            case 98: case 99: result.Format = DdsBlockFormat::BC7; break;
            default: throw std::runtime_error("Unsupported DDS format; expected BC1/3/5/7 UNORM or RGBA16F");
            }
            result.bSRgb = format == 72 || format == 78 || format == 99;
            offset = 148;
        }
        else throw std::runtime_error("Unsupported DDS FourCC; expected BC1/3/5/7 UNORM");

        if (result.Format == DdsBlockFormat::RGBA16Float && (flags & 8u) != 0 &&
            ReadU32(bytes, 20) != width * 8u)
            throw std::runtime_error("HDR DDS requires tightly packed RGBA16F rows");

        uint32_t maximumMipCount = 1;
        for (uint32_t dimension = std::max(width, height); dimension > 1; dimension >>= 1u)
            ++maximumMipCount;
        const uint32_t mipCount = std::max(1u, ReadU32(bytes, 28));
        if (mipCount > maximumMipCount)
            throw std::runtime_error("DDS mip count exceeds its dimensions");
        const size_t blockBytes = result.Format == DdsBlockFormat::BC1 ? 8u : 16u;
        for (uint32_t mip = 0; mip < mipCount; ++mip)
        {
            const bool bFloat = result.Format == DdsBlockFormat::RGBA16Float;
            const size_t rowPitch = bFloat ? size_t(width) * 8u : ((size_t(width) + 3u) / 4u) * blockBytes;
            const size_t slicePitch = rowPitch * (bFloat ? size_t(height) : ((size_t(height) + 3u) / 4u));
            if (offset > fileSize || slicePitch > fileSize - offset)
                throw std::runtime_error("Truncated DDS mip payload");
            result.Mips.push_back({ width, height, offset, rowPitch, slicePitch });
            offset += slicePitch;
            width = std::max(1u, width >> 1u);
            height = std::max(1u, height >> 1u);
        }
        return result;
    }

    DdsTextureData DdsTextureData::Parse(std::span<const std::byte> bytes)
    {
        DdsTextureData result = ParseDdsMetadata(bytes, bytes.size());
        const DdsMipData& lastMip = result.Mips.back();
        result.Bytes.assign(bytes.begin(), bytes.begin() + lastMip.Offset + lastMip.SlicePitch);
        return result;
    }

    DdsTextureData DdsTextureData::Load(const std::filesystem::path& path)
    {
        std::vector<std::byte> bytes;
        if (auto staged = AssetReadAhead::GetInstance().Take(path))
            bytes = std::move(*staged);
        else
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) throw std::runtime_error("Failed to open DDS asset");
            const std::streamoff length = stream.tellg();
            if (length < 128 || length > static_cast<std::streamoff>(c_MaxDdsBytes))
                throw std::runtime_error("Invalid DDS asset size");
            bytes.resize(static_cast<size_t>(length));
            stream.seekg(0);
            if (!stream.read(reinterpret_cast<char*>(bytes.data()), length))
                throw std::runtime_error("Failed to read complete DDS asset");
        }
        DdsTextureData result = ParseDdsMetadata(bytes, bytes.size());
        const DdsMipData& lastMip = result.Mips.back();
        bytes.resize(lastMip.Offset + lastMip.SlicePitch);
        result.Bytes = std::move(bytes);
        return result;
    }

    DdsTextureData DdsTextureData::LoadMipTail(const std::filesystem::path& path,
        uint32_t maximumDimension, uint32_t firstResidentMip)
    {
        auto staged = AssetReadAhead::GetInstance().Take(path);
        std::ifstream stream;
        DdsTextureData result;
        if (staged)
            result = ParseDdsMetadata(*staged, staged->size());
        else
        {
            stream.open(path, std::ios::binary | std::ios::ate);
            if (!stream) throw std::runtime_error("Failed to open DDS mip source");
            const std::streamoff length = stream.tellg();
            if (length < 128 || length > static_cast<std::streamoff>(c_MaxDdsBytes))
                throw std::runtime_error("Invalid DDS asset size");
            std::array<std::byte, 148> header{};
            stream.seekg(0);
            if (!stream.read(reinterpret_cast<char*>(header.data()), 128))
                throw std::runtime_error("Failed to read DDS header");
            size_t headerSize = 128;
            if (ReadU32(header, 84) == FourCC('D', 'X', '1', '0'))
            {
                if (!stream.read(reinterpret_cast<char*>(header.data() + 128), 20))
                    throw std::runtime_error("Truncated DDS DX10 header");
                headerSize = 148;
            }
            result = ParseDdsMetadata(std::span(header).first(headerSize), static_cast<size_t>(length));
            result.FileBytesRead = headerSize;
        }
        if (!result.HasCompleteMipChain())
            throw std::runtime_error("DDS streaming requires a complete offline mip chain");
        size_t firstMip = 0;
        if (maximumDimension != 0)
            while (firstMip + 1 < result.Mips.size() &&
                std::max(result.Mips[firstMip].Width, result.Mips[firstMip].Height) > maximumDimension)
                ++firstMip;
        result.SourceWidth = result.Mips[firstMip].Width;
        result.SourceHeight = result.Mips[firstMip].Height;
        result.FirstResidentMip = std::min(firstResidentMip,
            static_cast<uint32_t>(result.Mips.size() - firstMip - 1));
        firstMip += result.FirstResidentMip;
        const size_t start = result.Mips[firstMip].Offset;
        const auto& last = result.Mips.back();
        const size_t count = last.Offset + last.SlicePitch - start;
        result.Bytes.resize(count);
        if (staged)
            std::copy_n(staged->data() + start, count, result.Bytes.data());
        else
        {
            stream.seekg(static_cast<std::streamoff>(start));
            if (!stream.read(reinterpret_cast<char*>(result.Bytes.data()), static_cast<std::streamsize>(count)))
                throw std::runtime_error("Failed to read DDS mip tail");
            result.FileBytesRead += count;
        }
        result.Mips.erase(result.Mips.begin(), result.Mips.begin() + firstMip);
        for (auto& mip : result.Mips) mip.Offset -= start;
        return result;
    }
}
