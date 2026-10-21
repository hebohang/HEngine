#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace HEngine::PhysicsCollisionFormat
{
    inline constexpr uint64_t MagicV1 = 0x314853454D434548ull;
    inline constexpr uint64_t MagicV2 = 0x324853454D434548ull;
    inline constexpr size_t MaxBytes = 128u * 1024u * 1024u;
    struct Document
    {
        uint64_t Abi = 0;
        uint64_t VertexCount = 0;
        std::span<const uint8_t> Triangles;
        std::span<const uint8_t> Payload;
        bool bPortable = false;
    };
    inline uint64_t ReadLittle(std::span<const uint8_t> bytes, size_t offset, size_t count)
    {
        if (count > 8 || offset > bytes.size() || count > bytes.size() - offset)
            throw std::runtime_error("Truncated collision header.");
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i) value |= uint64_t(bytes[offset + i]) << (8 * i);
        return value;
    }
    inline void AppendLittle(std::string& bytes, uint64_t value, size_t count)
    {
        for (size_t i = 0; i < count; ++i) bytes.push_back(char((value >> (8 * i)) & 255));
    }
    inline Document Parse(std::span<const uint8_t> bytes)
    {
        if (bytes.size() < 17 || bytes.size() > MaxBytes)
            throw std::runtime_error("Invalid cooked collision size.");
        const auto magic = ReadLittle(bytes, 0, 8);
        Document result;
        result.Abi = ReadLittle(bytes, 8, 8);
        if (magic == MagicV1) { result.Payload = bytes.subspan(16); return result; }
        if (magic != MagicV2) throw std::runtime_error("Unsupported collision schema; reimport the source.");
        result.bPortable = true;
        result.VertexCount = ReadLittle(bytes, 16, 8);
        const auto payloadBytes = ReadLittle(bytes, 24, 8);
        if (!result.VertexCount || result.VertexCount % 3 || result.VertexCount > (MaxBytes - 32) / 12)
            throw std::runtime_error("Invalid portable collision triangle count.");
        const size_t triangleBytes = static_cast<size_t>(result.VertexCount) * 12;
        if (bytes.size() < 32 || triangleBytes > bytes.size() - 32 || !payloadBytes ||
            payloadBytes != bytes.size() - 32 - triangleBytes)
            throw std::runtime_error("Invalid portable collision payload lengths.");
        result.Triangles = bytes.subspan(32, triangleBytes);
        result.Payload = bytes.subspan(32 + triangleBytes);
        return result;
    }
}
