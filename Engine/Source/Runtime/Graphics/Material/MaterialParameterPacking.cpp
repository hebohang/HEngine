#include "Hepch.h"

#include "MaterialParameterPacking.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace HEngine
{
    namespace
    {
        struct ResolvedMaterialParameter
        {
            const MaterialShaderProperty* Property = nullptr;
            std::array<float, 4> Numeric{};
            bool Boolean = false;
            std::int32_t Integer = 0;
            std::string TexturePath;
            int TextureUvSet = 0;
            std::string Fallback;
        };

        class PackedDataHashBuilder final
        {
        public:
            void AddUnsigned(std::uint64_t value) noexcept
            {
                for (std::uint32_t byteIndex = 0; byteIndex < sizeof(value); ++byteIndex)
                {
                    AddByte(static_cast<std::uint8_t>(value & 0xffu));
                    value >>= 8u;
                }
            }

            void AddString(std::string_view value) noexcept
            {
                AddUnsigned(static_cast<std::uint64_t>(value.size()));
                for (const unsigned char character : value)
                    AddByte(character);
            }

            void AddBytes(std::span<const std::byte> bytes) noexcept
            {
                AddUnsigned(static_cast<std::uint64_t>(bytes.size()));
                for (const std::byte byte : bytes)
                    AddByte(std::to_integer<std::uint8_t>(byte));
            }

            [[nodiscard]] std::string Finish() const
            {
                constexpr char c_HexDigits[] = "0123456789abcdef";
                std::string result(16, '0');
                std::uint64_t value = mValue;
                for (std::size_t index = result.size(); index > 0; --index)
                {
                    result[index - 1] = c_HexDigits[value & 0x0fu];
                    value >>= 4u;
                }
                return result;
            }

        private:
            void AddByte(std::uint8_t value) noexcept
            {
                mValue ^= value;
                mValue *= 1099511628211ull;
            }

            std::uint64_t mValue = 14695981039346656037ull;
        };

        void AddError(
            MaterialParameterPackingResult& result,
            const MaterialShaderManifest& manifest,
            std::string code,
            std::string message)
        {
            result.Diagnostics.push_back({
                .Severity = MaterialShaderDiagnosticSeverity::Error,
                .Code = std::move(code),
                .Message = std::move(message),
                .Source = manifest.SourcePath,
            });
        }

        void AddWarning(
            MaterialParameterPackingResult& result,
            const MaterialShaderManifest& manifest,
            std::string code,
            std::string message)
        {
            result.Diagnostics.push_back({
                .Severity = MaterialShaderDiagnosticSeverity::Warning,
                .Code = std::move(code),
                .Message = std::move(message),
                .Source = manifest.SourcePath,
            });
        }

        [[nodiscard]] bool HasErrors(const MaterialParameterPackingResult& result) noexcept
        {
            return std::ranges::any_of(result.Diagnostics, [](const MaterialShaderDiagnostic& diagnostic)
            {
                return diagnostic.Severity == MaterialShaderDiagnosticSeverity::Error;
            });
        }

        [[nodiscard]] std::size_t NumericComponentCount(
            MaterialShaderPropertyType type) noexcept
        {
            switch (type)
            {
                case MaterialShaderPropertyType::Float: return 1;
                case MaterialShaderPropertyType::Float2: return 2;
                case MaterialShaderPropertyType::Float3: return 3;
                case MaterialShaderPropertyType::Color: return 4;
                default: return 0;
            }
        }

        [[nodiscard]] std::string ToLowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
            {
                if (character >= 'A' && character <= 'Z')
                    return static_cast<char>(character - 'A' + 'a');
                return static_cast<char>(character);
            });
            return value;
        }

        [[nodiscard]] bool IsBuiltinTextureToken(std::string_view value)
        {
            const std::string lower = ToLowerAscii(std::string(value));
            return lower == "white" || lower == "black" || lower == "normal";
        }

        [[nodiscard]] bool TryNormalizeTextureReference(
            std::string_view value,
            bool bAllowEmpty,
            std::string& normalized,
            std::string& error)
        {
            normalized.clear();
            error.clear();
            if (value.empty())
            {
                if (bAllowEmpty)
                    return true;
                error = "Texture fallback must not be empty.";
                return false;
            }
            if (IsBuiltinTextureToken(value))
            {
                normalized = ToLowerAscii(std::string(value));
                return true;
            }

            const Utils::PathValidationResult validation =
                Utils::TryNormalizeProjectRelativePath(value, normalized);
            if (!validation)
            {
                error = validation.Message;
                return false;
            }
            return true;
        }

        [[nodiscard]] const MaterialParameterValue* FindValue(
            const MaterialShaderManifest& manifest,
            const MaterialShaderProperty& property,
            std::span<const MaterialParameterValue> values,
            MaterialParameterPackingResult& result)
        {
            const MaterialParameterValue* match = nullptr;
            for (const MaterialParameterValue& value : values)
            {
                if (value.StableId.empty() || value.StableId != property.StableId)
                    continue;
                if (match)
                {
                    AddError(result, manifest, "Packing.DuplicateValue",
                        "More than one value matches stable id '" + property.StableId + "'.");
                    return nullptr;
                }
                match = &value;
            }
            if (match)
                return match;

            for (const MaterialParameterValue& value : values)
            {
                if (value.Name.empty() || value.Name != property.Name)
                    continue;

                const bool bStableIdTargetsKnownProperty = !value.StableId.empty() &&
                    std::ranges::any_of(manifest.Properties, [&value](const MaterialShaderProperty& candidate)
                    {
                        return candidate.StableId == value.StableId;
                    });
                if (bStableIdTargetsKnownProperty)
                    continue;
                if (match)
                {
                    AddError(result, manifest, "Packing.DuplicateValue",
                        "More than one value matches property name '" + property.Name + "'.");
                    return nullptr;
                }
                match = &value;
            }
            return match;
        }

        [[nodiscard]] bool TryConvertFloat(double value, float& converted) noexcept
        {
            if (!std::isfinite(value) ||
                value < -static_cast<double>(std::numeric_limits<float>::max()) ||
                value > static_cast<double>(std::numeric_limits<float>::max()))
            {
                return false;
            }
            converted = static_cast<float>(value);
            return std::isfinite(converted);
        }

        void ValidateManifestLayout(
            const MaterialShaderManifest& manifest,
            std::span<const MaterialParameterValue> values,
            MaterialParameterPackingResult& result)
        {
            if (manifest.Properties.size() > MaterialParameterMaximumPropertyCount)
            {
                AddError(result, manifest, "Packing.PropertyLimit",
                    "Material parameter property count exceeds 64.");
            }
            if (values.size() > MaterialParameterMaximumPropertyCount)
            {
                AddError(result, manifest, "Packing.ValueLimit",
                    "Material parameter input value count exceeds 64.");
            }
            if (manifest.ParameterBlock.ByteSize > MaterialParameterMaximumByteSize)
            {
                AddError(result, manifest, "Packing.ParameterBlockLimit",
                    "Material parameter block exceeds 1024 bytes.");
            }
            if (manifest.ParameterBlock.Alignment != MaterialShaderParameterAlignment ||
                manifest.ParameterBlock.ByteSize % MaterialShaderParameterAlignment != 0)
            {
                AddError(result, manifest, "Packing.ParameterBlockAlignment",
                    "Material parameter block must use 16-byte alignment and size.");
            }
            const std::uint64_t expectedBlockSize =
                static_cast<std::uint64_t>(manifest.Properties.size()) *
                MaterialShaderParameterAlignment;
            if (expectedBlockSize != manifest.ParameterBlock.ByteSize)
            {
                AddError(result, manifest, "Packing.ParameterBlockSize",
                    "Material parameter block must contain exactly one 16-byte row per property.");
            }

            std::size_t textureCount = 0;
            std::unordered_set<std::string> stableIds;
            std::unordered_set<std::string> names;
            std::unordered_set<std::uint32_t> rows;
            for (const MaterialShaderProperty& property : manifest.Properties)
            {
                if (property.StableId.empty() || !stableIds.insert(property.StableId).second)
                {
                    AddError(result, manifest, "Packing.PropertyStableId",
                        "Material parameter stable ids must be non-empty and unique.");
                }
                if (property.Name.empty() || !names.insert(property.Name).second)
                {
                    AddError(result, manifest, "Packing.PropertyName",
                        "Material parameter names must be non-empty and unique.");
                }
                if (property.ArrayCount != 1 || !rows.insert(property.RowOffset).second)
                {
                    AddError(result, manifest, "Packing.PropertyRow",
                        "Each material parameter must occupy one unique row.");
                }

                const std::uint32_t expectedByteSize =
                    MaterialShaderPropertyByteSize(property.Type);
                const std::uint64_t expectedByteOffset =
                    static_cast<std::uint64_t>(property.RowOffset) *
                    MaterialShaderParameterAlignment;
                const std::uint64_t byteEnd =
                    static_cast<std::uint64_t>(property.ByteOffset) + property.ByteSize;
                if (expectedByteSize == 0 || property.ByteSize != expectedByteSize ||
                    property.ByteOffset != expectedByteOffset ||
                    byteEnd > manifest.ParameterBlock.ByteSize)
                {
                    AddError(result, manifest, "Packing.PropertyLayout",
                        "Property '" + property.Name + "' has an invalid offset or byte size.");
                }
                if (property.Type == MaterialShaderPropertyType::Texture2D)
                    ++textureCount;
            }
            if (textureCount > MaterialParameterMaximumTextureCount)
            {
                AddError(result, manifest, "Packing.TextureLimit",
                    "Material parameter texture count exceeds 16.");
            }
        }

        [[nodiscard]] std::vector<ResolvedMaterialParameter> ResolveValues(
            const MaterialShaderManifest& manifest,
            std::span<const MaterialParameterValue> values,
            MaterialParameterPackingResult& result)
        {
            std::vector<ResolvedMaterialParameter> resolved;
            resolved.reserve(manifest.Properties.size());
            for (const MaterialShaderProperty& property : manifest.Properties)
            {
                ResolvedMaterialParameter parameter;
                parameter.Property = &property;
                const MaterialParameterValue* value = FindValue(manifest, property, values, result);
                if (value && value->Type != property.Type)
                {
                    AddWarning(result, manifest, "Packing.TypeMigratedToDefault",
                        "Value for property '" + property.Name + "' does not match manifest type " +
                        ToString(property.Type) + "; the manifest default is used.");
                    value = nullptr;
                }

                const std::size_t numericCount = NumericComponentCount(property.Type);
                if (numericCount != 0)
                {
                    if (!value && property.Default.Numeric.size() != numericCount)
                    {
                        AddError(result, manifest, "Packing.NumericDefault",
                            "Property '" + property.Name + "' has an invalid numeric default.");
                    }
                    else
                    {
                        for (std::size_t component = 0; component < numericCount; ++component)
                        {
                            const double source = value
                                ? static_cast<double>(value->Numeric[component])
                                : property.Default.Numeric[component];
                            if (!TryConvertFloat(source, parameter.Numeric[component]))
                            {
                                AddError(result, manifest, "Packing.NumericValue",
                                    "Property '" + property.Name + "' contains a non-finite or out-of-range numeric value.");
                                break;
                            }
                        }
                    }
                }
                else if (property.Type == MaterialShaderPropertyType::Bool)
                {
                    parameter.Boolean = value ? value->Boolean : property.Default.Boolean;
                }
                else if (property.Type == MaterialShaderPropertyType::Int)
                {
                    const std::int64_t integer = value ? value->Integer : property.Default.Integer;
                    if (integer < std::numeric_limits<std::int32_t>::min() ||
                        integer > std::numeric_limits<std::int32_t>::max())
                    {
                        AddError(result, manifest, "Packing.IntegerRange",
                            "Property '" + property.Name + "' is outside the signed 32-bit material ABI range.");
                    }
                    else
                    {
                        parameter.Integer = static_cast<std::int32_t>(integer);
                    }
                }
                else if (property.Type == MaterialShaderPropertyType::Texture2D)
                {
                    parameter.TextureUvSet = value ? value->TextureUvSet : 0;
                    if (parameter.TextureUvSet < 0 || parameter.TextureUvSet > 1)
                        AddError(result, manifest, "Packing.TextureUvSet",
                            "Property '" + property.Name + "' requires UV0 or UV1.");
                    const std::string_view texturePath = value
                        ? std::string_view(value->TexturePath)
                        : std::string_view(property.Default.Texture);
                    if (!value && texturePath.empty())
                    {
                        AddError(result, manifest, "Packing.TextureDefault",
                            "Property '" + property.Name + "' has an empty texture default.");
                    }

                    std::string error;
                    if (!TryNormalizeTextureReference(
                            texturePath, value != nullptr, parameter.TexturePath, error))
                    {
                        AddError(result, manifest, "Packing.TexturePath",
                            "Property '" + property.Name + "' has an unsafe texture path: " + error);
                    }
                    if (!IsBuiltinTextureToken(property.Fallback))
                    {
                        AddError(result, manifest, "Packing.TextureFallback",
                            "Property '" + property.Name +
                            "' fallback must be one of white, black, or normal.");
                    }
                    else
                    {
                        parameter.Fallback = ToLowerAscii(property.Fallback);
                    }
                }
                resolved.push_back(std::move(parameter));
            }
            return resolved;
        }

        void WriteUint32LittleEndian(
            std::vector<std::byte>& destination,
            std::size_t offset,
            std::uint32_t value) noexcept
        {
            for (std::size_t byteIndex = 0; byteIndex < sizeof(value); ++byteIndex)
            {
                destination[offset + byteIndex] = static_cast<std::byte>(value & 0xffu);
                value >>= 8u;
            }
        }

        void WriteFloatLittleEndian(
            std::vector<std::byte>& destination,
            std::size_t offset,
            float value) noexcept
        {
            WriteUint32LittleEndian(destination, offset, std::bit_cast<std::uint32_t>(value));
        }

        [[nodiscard]] std::string BuildPackedDataHash(
            std::span<const std::byte> bytes,
            std::span<const MaterialTextureParameterFixup> fixups)
        {
            PackedDataHashBuilder hash;
            hash.AddString("HEngine.MaterialParameterPackedData.v1");
            hash.AddBytes(bytes);
            hash.AddUnsigned(static_cast<std::uint64_t>(fixups.size()));
            for (const MaterialTextureParameterFixup& fixup : fixups)
            {
                hash.AddString(fixup.StableId);
                hash.AddString(fixup.Name);
                hash.AddUnsigned(fixup.ByteOffset);
                hash.AddUnsigned(fixup.RowOffset);
                hash.AddString(fixup.AssetPath);
                hash.AddString(fixup.Fallback);
                hash.AddUnsigned(static_cast<std::uint64_t>(fixup.Semantic));
                hash.AddUnsigned(static_cast<std::uint64_t>(fixup.ColorSpace));
            }
            return hash.Finish();
        }
    }

    bool MaterialParameterPackingResult::Succeeded() const noexcept
    {
        return !HasErrors(*this);
    }

    MaterialParameterPackingResult PackMaterialParameters(
        const MaterialShaderManifest& manifest,
        std::span<const MaterialParameterValue> values)
    {
        static_assert(sizeof(MaterialParameterFloat4Row) == MaterialShaderParameterAlignment);

        MaterialParameterPackingResult result;
        ValidateManifestLayout(manifest, values, result);
        std::vector<ResolvedMaterialParameter> resolved = ResolveValues(manifest, values, result);
        if (HasErrors(result))
            return result;

        result.PackedBytes.resize(manifest.ParameterBlock.ByteSize, std::byte{ 0 });
        result.TextureFixups.reserve(std::ranges::count_if(
            manifest.Properties, [](const MaterialShaderProperty& property)
            {
                return property.Type == MaterialShaderPropertyType::Texture2D;
            }));
        for (const ResolvedMaterialParameter& parameter : resolved)
        {
            const MaterialShaderProperty& property = *parameter.Property;
            const std::size_t offset = property.ByteOffset;
            const std::size_t numericCount = NumericComponentCount(property.Type);
            if (numericCount != 0)
            {
                for (std::size_t component = 0; component < numericCount; ++component)
                {
                    WriteFloatLittleEndian(
                        result.PackedBytes,
                        offset + component * sizeof(float),
                        parameter.Numeric[component]);
                }
            }
            else if (property.Type == MaterialShaderPropertyType::Bool)
            {
                WriteUint32LittleEndian(
                    result.PackedBytes, offset, parameter.Boolean ? 1u : 0u);
            }
            else if (property.Type == MaterialShaderPropertyType::Int)
            {
                WriteUint32LittleEndian(
                    result.PackedBytes,
                    offset,
                    std::bit_cast<std::uint32_t>(parameter.Integer));
            }
            else if (property.Type == MaterialShaderPropertyType::Texture2D)
            {
                WriteUint32LittleEndian(result.PackedBytes, offset, 0u);
                // Descriptor and normal encoding occupy x/y; z carries the
                // authored UV set through the same per-instance parameter row.
                WriteUint32LittleEndian(result.PackedBytes, offset + 8,
                    static_cast<std::uint32_t>(parameter.TextureUvSet));
                result.TextureFixups.push_back({
                    .StableId = property.StableId,
                    .Name = property.Name,
                    .ByteOffset = property.ByteOffset,
                    .RowOffset = property.RowOffset,
                    .AssetPath = parameter.TexturePath,
                    .Fallback = parameter.Fallback,
                    .Semantic = property.TextureSemantic,
                    .ColorSpace = property.ColorSpace,
                });
            }
        }

        result.PackedFloat4Rows.resize(
            manifest.ParameterBlock.ByteSize / MaterialShaderParameterAlignment);
        if (!result.PackedBytes.empty())
        {
            std::memcpy(
                result.PackedFloat4Rows.data(),
                result.PackedBytes.data(),
                result.PackedBytes.size());
        }
        result.PackedDataHash = BuildPackedDataHash(
            result.PackedBytes,
            result.TextureFixups);
        return result;
    }
}
