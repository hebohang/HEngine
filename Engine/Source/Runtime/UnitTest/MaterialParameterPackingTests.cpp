#include "Hepch.h"

#include "Runtime/UnitTest/MaterialParameterPackingTests.h"

#include "Runtime/Graphics/Material/MaterialParameterPacking.h"
#include "Runtime/Graphics/Material/MaterialPipelineKey.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        template<typename T>
        concept HasPackedDataHashField = requires(T value)
        {
            value.PackedDataHash;
        };

        static_assert(!HasPackedDataHashField<MaterialPipelineDesc>);

        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        [[nodiscard]] MaterialShaderProperty MakeProperty(
            std::string stableId,
            std::string name,
            MaterialShaderPropertyType type,
            std::uint32_t row)
        {
            return {
                .StableId = std::move(stableId),
                .Name = name,
                .DisplayName = std::move(name),
                .Source = "Shaders/MaterialPackingTest.hlsl",
                .Line = row + 1,
                .Column = 1,
                .Type = type,
                .RowOffset = row,
                .ByteOffset = row * MaterialShaderParameterAlignment,
                .ByteSize = MaterialShaderPropertyByteSize(type),
            };
        }

        [[nodiscard]] MaterialShaderManifest MakeManifest()
        {
            MaterialShaderManifest manifest;
            manifest.SourcePath = "Shaders/MaterialPackingTest.hlsl";
            manifest.RecursiveSourceHash = std::string(64, 'a');
            manifest.BindingSignatureHash = std::string(64, 'b');

            MaterialShaderProperty scalar = MakeProperty(
                "roughness-id", "Roughness", MaterialShaderPropertyType::Float, 0);
            scalar.Default.Numeric = { 0.25 };
            manifest.Properties.push_back(std::move(scalar));

            MaterialShaderProperty float2 = MakeProperty(
                "uv-scale-id", "UvScale", MaterialShaderPropertyType::Float2, 1);
            float2.Default.Numeric = { 1.0, 1.0 };
            manifest.Properties.push_back(std::move(float2));

            MaterialShaderProperty float3 = MakeProperty(
                "direction-id", "Direction", MaterialShaderPropertyType::Float3, 2);
            float3.Default.Numeric = { 3.0, 4.0, 5.0 };
            manifest.Properties.push_back(std::move(float3));

            MaterialShaderProperty color = MakeProperty(
                "tint-id", "Tint", MaterialShaderPropertyType::Color, 3);
            color.Default.Numeric = { 0.1, 0.2, 0.3, 0.4 };
            manifest.Properties.push_back(std::move(color));

            MaterialShaderProperty boolean = MakeProperty(
                "enabled-id", "Enabled", MaterialShaderPropertyType::Bool, 4);
            boolean.Default.Boolean = true;
            manifest.Properties.push_back(std::move(boolean));

            MaterialShaderProperty integer = MakeProperty(
                "layer-id", "Layer", MaterialShaderPropertyType::Int, 5);
            integer.Default.Integer = -3;
            manifest.Properties.push_back(std::move(integer));

            MaterialShaderProperty texture = MakeProperty(
                "albedo-id", "Albedo", MaterialShaderPropertyType::Texture2D, 6);
            texture.Default.Texture = "Assets/Textures/Default.png";
            texture.Fallback = "white";
            texture.TextureSemantic = MaterialShaderTextureSemantic::BaseColor;
            texture.ColorSpace = MaterialShaderColorSpace::Srgb;
            manifest.Properties.push_back(std::move(texture));

            manifest.ParameterBlock.ByteSize = static_cast<std::uint32_t>(
                manifest.Properties.size() * MaterialShaderParameterAlignment);
            return manifest;
        }

        [[nodiscard]] std::uint32_t ReadUint32(
            const std::vector<std::byte>& bytes,
            std::size_t offset)
        {
            return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
                (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8u) |
                (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2])) << 16u) |
                (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3])) << 24u);
        }

        [[nodiscard]] float ReadFloat(
            const std::vector<std::byte>& bytes,
            std::size_t offset)
        {
            return std::bit_cast<float>(ReadUint32(bytes, offset));
        }

        [[nodiscard]] bool HasDiagnostic(
            const MaterialParameterPackingResult& result,
            std::string_view code)
        {
            return std::ranges::any_of(result.Diagnostics, [code](const MaterialShaderDiagnostic& diagnostic)
            {
                return diagnostic.Code == code;
            });
        }

        void RequireFailure(
            const MaterialParameterPackingResult& result,
            std::string_view code)
        {
            Require(!result.Succeeded(), "Invalid material parameter input unexpectedly succeeded.");
            Require(HasDiagnostic(result, code),
                std::string("Missing material parameter diagnostic: ") + std::string(code));
            Require(result.PackedBytes.empty() && result.PackedFloat4Rows.empty() &&
                    result.TextureFixups.empty() && result.PackedDataHash.empty(),
                "Failed material parameter packing exposed partially written output.");
        }

        [[nodiscard]] std::vector<MaterialParameterValue> MakeValues()
        {
            return {
                MaterialParameterValue{
                    .StableId = "roughness-id",
                    .Name = "IgnoredName",
                    .Type = MaterialShaderPropertyType::Float,
                    .Numeric = { 0.75f, 0.0f, 0.0f, 0.0f },
                },
                MaterialParameterValue{
                    .StableId = "retired-uv-id",
                    .Name = "UvScale",
                    .Type = MaterialShaderPropertyType::Float2,
                    .Numeric = { 2.0f, 3.0f, 0.0f, 0.0f },
                },
                MaterialParameterValue{
                    .StableId = "enabled-id",
                    .Type = MaterialShaderPropertyType::Bool,
                    .Boolean = false,
                },
                MaterialParameterValue{
                    .Name = "Layer",
                    .Type = MaterialShaderPropertyType::Int,
                    .Integer = 42,
                },
                MaterialParameterValue{
                    .StableId = "albedo-id",
                    .Type = MaterialShaderPropertyType::Texture2D,
                    .TexturePath = "Assets\\Textures\\Custom.png",
                },
            };
        }
    }

    void MaterialParameterPackingContract()
    {
        const MaterialShaderManifest manifest = MakeManifest();
        const std::vector<MaterialParameterValue> values = MakeValues();
        const MaterialParameterPackingResult packed = PackMaterialParameters(manifest, values);
        Require(packed.Succeeded(), "Valid typed material parameters failed to pack.");
        Require(packed.PackedBytes.size() == manifest.ParameterBlock.ByteSize,
            "Packed byte size did not equal manifest ParameterBlock.ByteSize.");
        Require(packed.PackedFloat4Rows.size() * sizeof(MaterialParameterFloat4Row) ==
                manifest.ParameterBlock.ByteSize,
            "Packed float4 row count did not equal manifest ParameterBlock.ByteSize.");
        Require(std::memcmp(
                packed.PackedFloat4Rows.data(),
                packed.PackedBytes.data(),
                packed.PackedBytes.size()) == 0,
            "Packed byte and float4-row views contain different payloads.");

        Require(ReadFloat(packed.PackedBytes, 0) == 0.75f,
            "Stable-id matching did not override the scalar value.");
        Require(ReadFloat(packed.PackedBytes, 16) == 2.0f &&
                ReadFloat(packed.PackedBytes, 20) == 3.0f,
            "Name fallback did not pack the float2 value.");
        Require(ReadFloat(packed.PackedBytes, 32) == 3.0f &&
                ReadFloat(packed.PackedBytes, 36) == 4.0f &&
                ReadFloat(packed.PackedBytes, 40) == 5.0f,
            "Missing float3 value did not use the manifest default.");
        Require(ReadUint32(packed.PackedBytes, 64) == 0,
            "Bool material parameter did not use the 32-bit ABI.");
        Require(ReadUint32(packed.PackedBytes, 80) == 42,
            "Int material parameter did not use the 32-bit ABI.");
        for (std::size_t offset : { 4u, 8u, 12u, 24u, 28u, 44u })
        {
            Require(ReadUint32(packed.PackedBytes, offset) == 0,
                "Material parameter row padding was not deterministic zero.");
        }
        for (std::size_t offset = 96; offset < 112; offset += sizeof(std::uint32_t))
        {
            Require(ReadUint32(packed.PackedBytes, offset) == 0,
                "Texture2D parameter row was not initialized to a zero descriptor placeholder.");
        }

        Require(packed.TextureFixups.size() == 1,
            "Texture2D parameter did not produce exactly one descriptor fixup.");
        const MaterialTextureParameterFixup& fixup = packed.TextureFixups.front();
        Require(fixup.StableId == "albedo-id" && fixup.Name == "Albedo" &&
                fixup.ByteOffset == 96 && fixup.RowOffset == 6 &&
                fixup.AssetPath == "Assets/Textures/Custom.png" &&
                fixup.Fallback == "white" &&
                fixup.Semantic == MaterialShaderTextureSemantic::BaseColor &&
                fixup.ColorSpace == MaterialShaderColorSpace::Srgb,
            "Texture descriptor fixup omitted manifest or normalized asset metadata.");

        const MaterialParameterPackingResult defaults = PackMaterialParameters(manifest, {});
        Require(defaults.Succeeded() && ReadFloat(defaults.PackedBytes, 0) == 0.25f &&
                ReadUint32(defaults.PackedBytes, 64) == 1u &&
                ReadUint32(defaults.PackedBytes, 80) ==
                    std::bit_cast<std::uint32_t>(std::int32_t{ -3 }) &&
                defaults.TextureFixups.front().AssetPath == "Assets/Textures/Default.png",
            "Missing values did not use typed manifest defaults.");

        const MaterialParameterPackingResult repeated = PackMaterialParameters(manifest, values);
        Require(repeated.Succeeded() && repeated.PackedBytes == packed.PackedBytes &&
                repeated.PackedFloat4Rows == packed.PackedFloat4Rows &&
                repeated.TextureFixups == packed.TextureFixups &&
                repeated.PackedDataHash == packed.PackedDataHash,
            "Identical material parameter inputs produced nondeterministic packed output.");

        std::vector<MaterialParameterValue> secondValues = values;
        secondValues.front().Numeric[0] = 0.5f;
        const MaterialParameterPackingResult secondInstance =
            PackMaterialParameters(manifest, secondValues);
        Require(secondInstance.Succeeded() &&
                secondInstance.PackedDataHash != packed.PackedDataHash,
            "Different material instance values produced the same packed-data hash.");

        std::vector<MaterialParameterValue> textureValues = values;
        textureValues.back().TexturePath = "Assets/Textures/Other.png";
        const MaterialParameterPackingResult textureInstance =
            PackMaterialParameters(manifest, textureValues);
        Require(textureInstance.Succeeded() &&
                textureInstance.PackedBytes == packed.PackedBytes &&
                textureInstance.PackedDataHash != packed.PackedDataHash,
            "Texture fixup identity was omitted from the packed-data hash.");

        const MaterialPipelineDesc pipelineDesc{
            .Program = {
                .RecursiveContentHash = manifest.RecursiveSourceHash,
                .Generation = 1,
                .MaterialAbiVersion = manifest.MaterialAbiVersion,
                .BindingSignature = manifest.BindingSignatureHash,
            },
            .Pass = MaterialPipelinePass::ForwardOpaque,
            .VertexPath = MaterialPipelineVertexPath::CpuStatic,
            .DescriptorTier = MaterialPipelineDescriptorTier::Baseline,
            .InputLayout = { .Value = 17 },
            .Framebuffer = {
                .ColorFormat = nvrhi::Format::RGBA16_FLOAT,
                .DepthFormat = nvrhi::Format::D32,
            },
        };
        const GraphicsPipelineKey firstInstancePipelineKey =
            BuildMaterialPipelineKey(pipelineDesc);
        const GraphicsPipelineKey secondInstancePipelineKey =
            BuildMaterialPipelineKey(pipelineDesc);
        Require(firstInstancePipelineKey == secondInstancePipelineKey &&
                packed.PackedDataHash != secondInstance.PackedDataHash,
            "Per-instance packed data was not isolated from the material pipeline key.");

        MaterialShaderManifest invalid = manifest;
        invalid.ParameterBlock.Alignment = 8;
        RequireFailure(PackMaterialParameters(invalid, {}), "Packing.ParameterBlockAlignment");
        invalid = manifest;
        invalid.Properties.front().ByteOffset = std::numeric_limits<std::uint32_t>::max();
        RequireFailure(PackMaterialParameters(invalid, {}), "Packing.PropertyLayout");
        invalid = manifest;
        invalid.Properties.front().ByteSize = 8;
        RequireFailure(PackMaterialParameters(invalid, {}), "Packing.PropertyLayout");
        invalid = manifest;
        invalid.ParameterBlock.ByteSize = 1040;
        RequireFailure(PackMaterialParameters(invalid, {}), "Packing.ParameterBlockLimit");

        invalid = manifest;
        while (invalid.Properties.size() <= MaterialParameterMaximumPropertyCount)
        {
            const std::uint32_t row = static_cast<std::uint32_t>(invalid.Properties.size());
            MaterialShaderProperty property = MakeProperty(
                "extra-" + std::to_string(row),
                "Extra" + std::to_string(row),
                MaterialShaderPropertyType::Float,
                row);
            property.Default.Numeric = { 0.0 };
            invalid.Properties.push_back(std::move(property));
        }
        invalid.ParameterBlock.ByteSize = static_cast<std::uint32_t>(
            invalid.Properties.size() * MaterialShaderParameterAlignment);
        RequireFailure(PackMaterialParameters(invalid, {}), "Packing.PropertyLimit");

        invalid = MaterialShaderManifest{};
        invalid.SourcePath = manifest.SourcePath;
        for (std::uint32_t row = 0; row < 17; ++row)
        {
            MaterialShaderProperty texture = MakeProperty(
                "texture-" + std::to_string(row),
                "Texture" + std::to_string(row),
                MaterialShaderPropertyType::Texture2D,
                row);
            texture.Default.Texture = "white";
            texture.Fallback = "white";
            invalid.Properties.push_back(std::move(texture));
        }
        invalid.ParameterBlock.ByteSize = static_cast<std::uint32_t>(
            invalid.Properties.size() * MaterialShaderParameterAlignment);
        RequireFailure(PackMaterialParameters(invalid, {}), "Packing.TextureLimit");

        std::vector<MaterialParameterValue> mismatched = {
            MaterialParameterValue{
                .StableId = "roughness-id",
                .Type = MaterialShaderPropertyType::Int,
                .Integer = 1,
            },
        };
        const MaterialParameterPackingResult migrated =
            PackMaterialParameters(manifest, mismatched);
        Require(migrated.Succeeded() && ReadFloat(migrated.PackedBytes, 0) == 0.25f,
            "An incompatible hot-reload value did not migrate to the manifest default.");
        Require(std::ranges::any_of(migrated.Diagnostics, [](const auto& diagnostic)
        {
            return diagnostic.Code == "Packing.TypeMigratedToDefault" &&
                diagnostic.Severity == MaterialShaderDiagnosticSeverity::Warning;
        }), "Incompatible hot-reload value migration did not report a warning.");

        std::vector<MaterialParameterValue> unsafeTexture = {
            MaterialParameterValue{
                .StableId = "albedo-id",
                .Type = MaterialShaderPropertyType::Texture2D,
                .TexturePath = "../Escape.png",
            },
        };
        RequireFailure(PackMaterialParameters(manifest, unsafeTexture), "Packing.TexturePath");
    }
}
