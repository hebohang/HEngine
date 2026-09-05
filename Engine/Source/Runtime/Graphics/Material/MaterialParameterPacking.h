#pragma once

#include "MaterialShaderManifest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace HEngine
{
    struct MaterialParameterValue
    {
        std::string StableId;
        std::string Name;
        MaterialShaderPropertyType Type = MaterialShaderPropertyType::Float;
        std::array<float, 4> Numeric{};
        bool Boolean = false;
        std::int64_t Integer = 0;
        std::string TexturePath;
    };

    using MaterialParameterFloat4Row = std::array<float, 4>;

    struct MaterialTextureParameterFixup
    {
        std::string StableId;
        std::string Name;
        std::uint32_t ByteOffset = 0;
        std::uint32_t RowOffset = 0;
        std::string AssetPath;
        std::string Fallback;
        MaterialShaderTextureSemantic Semantic = MaterialShaderTextureSemantic::None;
        MaterialShaderColorSpace ColorSpace = MaterialShaderColorSpace::Linear;

        [[nodiscard]] bool operator==(const MaterialTextureParameterFixup&) const = default;
    };

    struct MaterialParameterPackingResult
    {
        std::vector<std::byte> PackedBytes;
        std::vector<MaterialParameterFloat4Row> PackedFloat4Rows;
        std::vector<MaterialTextureParameterFixup> TextureFixups;
        std::string PackedDataHash;
        std::vector<MaterialShaderDiagnostic> Diagnostics;

        [[nodiscard]] bool Succeeded() const noexcept;
    };

    // The returned buffers are all-or-nothing. On success PackedBytes is
    // exactly ParameterBlock.ByteSize and PackedFloat4Rows is the same payload
    // viewed as 16-byte rows. Instance data never participates in pipeline keys.
    [[nodiscard]] MaterialParameterPackingResult PackMaterialParameters(
        const MaterialShaderManifest& manifest,
        std::span<const MaterialParameterValue> values);
}
