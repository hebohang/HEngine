#include "Hepch.h"
#include "_generated/serializer/TextureSampling.gen.h"

#include "Material.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/Material/MaterialShaderPath.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/EnumUtils/EnumUtils.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "_generated/serializer/Material.gen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace HEngine
{
    namespace
    {
        std::string ToLowerCopy(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        std::string NormalizePath(std::string path)
        {
            for (char& character : path)
            {
                if (character == '/')
                    character = '\\';
            }
            return path;
        }

        bool IsBuiltinTextureToken(const std::string& token)
        {
            const std::string lower = ToLowerCopy(token);
            return lower == "white" || lower == "black" || lower == "normal";
        }

        std::string CollapseBuiltinTexturePath(const std::string& value)
        {
            const std::string normalized = ToLowerCopy(NormalizePath(value));
            if (normalized == "assets\\textures\\white.png")
                return "white";
            if (normalized == "assets\\textures\\black.png")
                return "black";
            if (normalized == "assets\\textures\\defaultnormal.png")
                return "normal";
            return NormalizePath(value);
        }

        std::string ShaderPathToPassName(const std::string& shaderPath)
        {
            if (shaderPath.empty())
                return "BasePBR";
            return std::filesystem::path(shaderPath).stem().string();
        }

        bool TryCanonicalizeShaderPath(const std::string& shaderPath, std::string& outPath)
        {
            std::string error;
            return TryNormalizeMaterialShaderPath(shaderPath, outPath, error);
        }

        [[nodiscard]] std::string CanonicalizeMaterialResourcePath(
            std::string_view materialPath)
        {
            std::string normalized;
            const Utils::PathValidationResult validation =
                Utils::TryNormalizeProjectRelativePath(materialPath, normalized);
            if (!validation ||
                ToLowerCopy(Utils::PathToUtf8(
                    Utils::Utf8ToPath(normalized).extension())) != ".hmat")
            {
                throw std::invalid_argument(
                    "Material resource path must be a safe project-relative .hmat path: " +
                    std::string(materialPath));
            }
            return normalized;
        }

        [[nodiscard]] std::filesystem::path ResolveMaterialLoadPath(
            const std::string& materialPath)
        {
            const std::filesystem::path fullPath =
                ConfigManager::GetFullPath(materialPath);
            // GetFullPath preserves the project-override/engine-fallback
            // lookup order. Validate the physically resolved result here so
            // direct Material construction cannot bypass the cache boundary
            // through a junction or symbolic link.
            (void)ConfigManager::GetContentRootForPath(fullPath);
            return fullPath;
        }

#if WITH_EDITOR
        [[nodiscard]] std::filesystem::path ResolveMaterialSavePath(
            const std::string& materialPath)
        {
            const std::filesystem::path fullPath =
                ConfigManager::GetProjectFullPath(materialPath);
            const std::filesystem::path physicalRoot =
                ConfigManager::GetContentRootForPath(fullPath);
            if (physicalRoot != ConfigManager::GetInstance().GetRootFolder())
            {
                throw std::invalid_argument(
                    "Material saves must remain inside the physical project content root.");
            }
            return fullPath;
        }
#endif

        std::string CanonicalPropertyName(std::string name)
        {
            static const std::unordered_map<std::string, std::string> aliases = {
                { "g_AlbedoMap", "g_BaseColorMap" },
                { "g_BaseColorFactor", "baseColorTint" },
                { "g_MetallicFactor", "metallic" },
                { "g_RoughnessFactor", "roughness" },
                { "g_NormalStrength", "normalStrength" },
            };
            if (const auto iterator = aliases.find(name); iterator != aliases.end())
                return iterator->second;
            return name;
        }

        std::string PropertyTypeName(MaterialPropertyType type)
        {
            switch (type)
            {
            case MaterialPropertyType::Texture: return "texture2d";
            case MaterialPropertyType::Float: return "float";
            case MaterialPropertyType::Color: return "color";
            case MaterialPropertyType::Float2: return "float2";
            case MaterialPropertyType::Float3: return "float3";
            case MaterialPropertyType::Bool: return "bool";
            case MaterialPropertyType::Int: return "int";
            }
            return "unknown";
        }

        bool IsTypeCompatible(const std::string& serializedType, MaterialPropertyType runtimeType)
        {
            const std::string normalized = ToLowerCopy(serializedType);
            if (runtimeType == MaterialPropertyType::Texture)
                return normalized == "texture" || normalized == "texture2d";
            if (runtimeType == MaterialPropertyType::Color)
                return normalized == "color" || normalized == "float4";
            return normalized == PropertyTypeName(runtimeType);
        }

        bool TryReadVector(const Json& value, glm::vec4& outValue, size_t minimumSize, size_t maximumSize)
        {
            if (!value.is_array() || value.size() < minimumSize || value.size() > maximumSize)
                return false;
            glm::vec4 parsed(0.0f);
            if (minimumSize >= 4)
                parsed.w = 1.0f;
            for (size_t index = 0; index < value.size(); ++index)
            {
                if (!value[index].is_number())
                    return false;
                const float component = value[index].get<float>();
                if (!std::isfinite(component))
                    return false;
                parsed[index] = component;
            }
            if (maximumSize == 4 && value.size() == 3)
                parsed.w = 1.0f;
            outValue = parsed;
            return true;
        }

        bool TryNormalizeTextureReference(
            const std::string& value,
            bool bAllowEmpty,
            std::string& outValue)
        {
            if (value.empty())
            {
                if (!bAllowEmpty)
                    return false;
                outValue.clear();
                return true;
            }
            if (IsBuiltinTextureToken(value))
            {
                outValue = ToLowerCopy(value);
                return true;
            }

            std::string normalized;
            if (!Utils::TryNormalizeProjectRelativePath(value, normalized) ||
                !ToLowerCopy(normalized).starts_with("assets/") ||
                Utils::Utf8ToPath(normalized).extension().empty())
            {
                return false;
            }
            outValue = CollapseBuiltinTexturePath(normalized);
            return true;
        }

        bool TryReadTextureEntry(
            const Json& entry,
            const std::string& defaultFallback,
            std::string& outValue,
            std::string* outFallback = nullptr)
        {
            if (!entry.is_object())
                return false;
            if (entry.contains("Path") && !entry["Path"].is_string())
                return false;
            if (entry.contains("Fallback") && !entry["Fallback"].is_string())
                return false;

            const std::string path = entry.value("Path", std::string{});
            const std::string fallback = entry.value("Fallback", defaultFallback);
            std::string normalizedFallback;
            if (!TryNormalizeTextureReference(fallback, true, normalizedFallback))
                return false;
            if (outFallback)
                *outFallback = normalizedFallback;

            if (path.empty())
            {
                outValue = std::move(normalizedFallback);
                return true;
            }
            return TryNormalizeTextureReference(path, false, outValue);
        }

        bool TryReadRuntimePropertyValue(
            MaterialRuntimeProperty& property,
            const Json& value)
        {
            switch (property.Decl.Type)
            {
            case MaterialPropertyType::Float:
                if (value.is_number())
                {
                    const float parsed = value.get<float>();
                    if (std::isfinite(parsed))
                    {
                        property.FloatValue = parsed;
                        return true;
                    }
                }
                return false;
            case MaterialPropertyType::Float2:
                return TryReadVector(value, property.ColorValue, 2, 2);
            case MaterialPropertyType::Float3:
                return TryReadVector(value, property.ColorValue, 3, 3);
            case MaterialPropertyType::Color:
                return TryReadVector(value, property.ColorValue, 3, 4);
            case MaterialPropertyType::Bool:
                if (value.is_boolean())
                {
                    property.BoolValue = value.get<bool>();
                    return true;
                }
                return false;
            case MaterialPropertyType::Int:
                if (value.is_number_integer())
                {
                    property.IntValue = value.get<int>();
                    return true;
                }
                return false;
            case MaterialPropertyType::Texture:
                return false;
            }
            return false;
        }

        std::string OrphanSerializedName(
            const std::string& storageKey,
            const Json& entry)
        {
            return entry.is_object() && entry.contains("SerializedName") &&
                    entry["SerializedName"].is_string()
                ? entry["SerializedName"].get<std::string>()
                : storageKey;
        }

        std::string PreserveOrphan(
            Json& orphanedParameters,
            const std::string& serializedName,
            Json orphan,
            std::string_view reason,
            std::string_view duplicateOf = {})
        {
            if (!orphanedParameters.is_object())
                orphanedParameters = Json::object();
            if (!orphan.is_object())
                orphan = Json{ { "Value", std::move(orphan) } };
            orphan["SerializedName"] = serializedName;
            orphan["Reason"] = reason;
            if (!duplicateOf.empty())
                orphan["DuplicateOf"] = duplicateOf;

            // Loading and saving an unchanged invalid entry must not create a
            // fresh generation on every round trip.
            for (auto iterator = orphanedParameters.begin();
                iterator != orphanedParameters.end();
                ++iterator)
            {
                Json normalizedExisting = iterator.value();
                if (!normalizedExisting.is_object())
                    continue;
                if (!normalizedExisting.contains("SerializedName"))
                {
                    normalizedExisting["SerializedName"] =
                        OrphanSerializedName(iterator.key(), iterator.value());
                }
                if (normalizedExisting == orphan)
                    return iterator.key();
            }

            std::string storageKey = serializedName;
            if (!orphanedParameters.contains(storageKey))
            {
                orphanedParameters[storageKey] = std::move(orphan);
                return storageKey;
            }
            for (size_t generation = 2;; ++generation)
            {
                storageKey = serializedName + "#" + std::to_string(generation);
                if (!orphanedParameters.contains(storageKey))
                {
                    orphanedParameters[storageKey] = std::move(orphan);
                    return storageKey;
                }
            }
        }

        bool StableIdentityMatches(
            const MaterialPropertyDecl& current,
            const MaterialPropertyDecl& previous)
        {
            return !current.StableId.empty() &&
                current.StableId == previous.StableId;
        }

        bool CompatibleNameIdentityMatches(
            const MaterialPropertyDecl& current,
            const MaterialPropertyDecl& previous)
        {
            // Two different non-empty stable ids represent different property
            // generations even when their display/source names are reused.
            if (!current.StableId.empty() && !previous.StableId.empty())
                return false;
            return current.Name == previous.Name ||
                CanonicalPropertyName(current.Name) ==
                    CanonicalPropertyName(previous.Name);
        }

        bool TryRestoreRuntimePropertyFromOrphans(
            MaterialRuntimeProperty& property,
            Json& orphanedParameters)
        {
            if (!orphanedParameters.is_object())
                return false;

            const auto tryRestore = [&](bool bStableIdPass) -> bool
            {
                for (auto iterator = orphanedParameters.begin();
                    iterator != orphanedParameters.end();
                    ++iterator)
                {
                    const Json& entry = iterator.value();
                    if (!entry.is_object() ||
                        entry.value("Reason", std::string{}) == "duplicate-stable-id")
                    {
                        continue;
                    }
                    const std::string stableId =
                        entry.contains("StableId") && entry["StableId"].is_string()
                        ? entry["StableId"].get<std::string>()
                        : std::string{};
                    if (bStableIdPass)
                    {
                        if (property.Decl.StableId.empty() ||
                            property.Decl.StableId != stableId)
                        {
                            continue;
                        }
                    }
                    else
                    {
                        if (!property.Decl.StableId.empty() && !stableId.empty())
                            continue;
                        const std::string serializedName =
                            OrphanSerializedName(iterator.key(), entry);
                        if (property.Decl.Name != serializedName &&
                            CanonicalPropertyName(property.Decl.Name) !=
                                CanonicalPropertyName(serializedName))
                        {
                            continue;
                        }
                    }

                    if (!entry.contains("Type") || !entry["Type"].is_string() ||
                        !IsTypeCompatible(
                            entry["Type"].get<std::string>(), property.Decl.Type))
                    {
                        continue;
                    }

                    bool bRestored = false;
                    if (property.Decl.Type == MaterialPropertyType::Texture)
                    {
                        bRestored = TryReadTextureEntry(
                            entry,
                            property.Decl.DefaultTextureToken,
                            property.TextureValue);
                    }
                    else if (entry.contains("Value"))
                    {
                        bRestored = TryReadRuntimePropertyValue(
                            property, entry["Value"]);
                    }
                    if (!bRestored)
                        continue;
                    if (property.Decl.Type == MaterialPropertyType::Texture && entry.contains("Sampling")) {
                        Serializer::Deserialize(entry.at("Sampling"), property.Sampling);
                        (void)PackTextureSampling(property.Sampling);
                    }


                    orphanedParameters.erase(iterator);
                    return true;
                }
                return false;
            };

            return tryRestore(true) || tryRestore(false);
        }

        MaterialShaderManifestLoadResult LoadPublishedMaterialManifest(
            const std::string& shaderPath)
        {
            std::string normalized;
            std::string error;
            std::filesystem::path manifestPath;
            if (!TryResolvePublishedMaterialShaderManifestPath(
                    ConfigManager::GetInstance().GetEngineGeneratedContentFolder(),
                    shaderPath,
                    manifestPath,
                    normalized,
                    error))
            {
                MaterialShaderManifestLoadResult result;
                result.Diagnostics.push_back({
                    .Severity = MaterialShaderDiagnosticSeverity::Error,
                    .Code = "Material.ManifestShaderPath",
                    .Message = "Could not resolve the published material manifest: " + error,
                    .Source = shaderPath,
                });
                return result;
            }
            return LoadMaterialShaderManifest(manifestPath);
        }

        bool MatchesSemantic(
            MaterialTextureSemantic semantic,
            const MaterialRuntimeProperty& property)
        {
            const std::string& name = property.Decl.Name;
            switch (semantic)
            {
            case MaterialTextureSemantic::BaseColor:
                return name == "g_AlbedoMap" || name == "g_BaseColorMap" || property.Decl.TextureSlot == 0;
            case MaterialTextureSemantic::Normal:
                return name == "g_NormalMap";
            case MaterialTextureSemantic::MetallicRoughness:
                return name == "g_MetallicRoughnessMap";
            case MaterialTextureSemantic::Occlusion:
                return name == "g_AoMap";
            case MaterialTextureSemantic::Emissive:
                return name == "g_EmissiveMap";
            }
            return false;
        }

        MaterialRuntimeProperty BuildRuntimeProperty(
            const MaterialShaderProperty& source)
        {
            MaterialPropertyDecl declaration;
            declaration.Name = source.Name;
            declaration.StableId = source.StableId;
            declaration.DisplayName = source.DisplayName;
            declaration.Category = source.Group;
            declaration.ArrayCount = source.ArrayCount;
            declaration.RowOffset = source.RowOffset;
            declaration.ByteOffset = source.ByteOffset;
            declaration.ByteSize = source.ByteSize;
            declaration.SourceLine = source.Line;
            declaration.bHasRange = source.Range.bEnabled;
            declaration.RangeMin = static_cast<float>(source.Range.Minimum);
            declaration.RangeMax = static_cast<float>(source.Range.Maximum);
            declaration.DefaultTextureToken = source.Default.Texture.empty()
                ? source.Fallback
                : source.Default.Texture;
            declaration.TextureSemantic = ToString(source.TextureSemantic);
            declaration.ColorSpace = ToString(source.ColorSpace);
            declaration.TextureSlot = source.Binding.DescriptorIndex;
            declaration.RegisterSpace = source.Binding.RegisterSpace;
            switch (source.Type)
            {
            case MaterialShaderPropertyType::Float:
                declaration.Type = MaterialPropertyType::Float;
                break;
            case MaterialShaderPropertyType::Float2:
                declaration.Type = MaterialPropertyType::Float2;
                break;
            case MaterialShaderPropertyType::Float3:
                declaration.Type = MaterialPropertyType::Float3;
                break;
            case MaterialShaderPropertyType::Color:
                declaration.Type = MaterialPropertyType::Color;
                break;
            case MaterialShaderPropertyType::Bool:
                declaration.Type = MaterialPropertyType::Bool;
                declaration.DefaultBool = source.Default.Boolean;
                break;
            case MaterialShaderPropertyType::Int:
                declaration.Type = MaterialPropertyType::Int;
                declaration.DefaultInt = static_cast<int>(source.Default.Integer);
                break;
            case MaterialShaderPropertyType::Texture2D:
                declaration.Type = MaterialPropertyType::Texture;
                break;
            }
            for (size_t index = 0;
                index < source.Default.Numeric.size() && index < 4;
                ++index)
            {
                declaration.DefaultValue[index] =
                    static_cast<float>(source.Default.Numeric[index]);
            }

            MaterialRuntimeProperty property;
            property.Decl = std::move(declaration);
            property.FloatValue = property.Decl.DefaultValue.x;
            property.ColorValue = property.Decl.DefaultValue;
            property.IntValue = property.Decl.DefaultInt;
            property.BoolValue = property.Decl.DefaultBool;
            if (property.Decl.Type == MaterialPropertyType::Texture)
            {
                const std::string& token = property.Decl.DefaultTextureToken;
                property.TextureValue = IsBuiltinTextureToken(token)
                    ? ToLowerCopy(token)
                    : NormalizePath(token);
            }
            return property;
        }

        std::vector<MaterialRuntimeProperty> BuildRuntimeProperties(
            const MaterialShaderManifest& manifest)
        {
            std::vector<MaterialRuntimeProperty> properties;
            properties.reserve(manifest.Properties.size());
            for (const MaterialShaderProperty& source : manifest.Properties)
                properties.push_back(BuildRuntimeProperty(source));
            return properties;
        }

        bool ManifestMatchesShaderPath(
            const MaterialShaderManifest& manifest,
            const std::string& shaderPath)
        {
            std::string normalizedShader;
            if (!TryCanonicalizeShaderPath(shaderPath, normalizedShader))
                return false;

            constexpr std::string_view prefix = "Shaders/";
            std::string expectedSource = normalizedShader.substr(prefix.size());
            std::string manifestSource;
            if (!Utils::TryNormalizeProjectRelativePath(
                    manifest.SourcePath,
                    manifestSource))
            {
                return false;
            }
            if (ToLowerCopy(manifestSource).starts_with("shaders/"))
                manifestSource.erase(0, prefix.size());
            return ToLowerCopy(expectedSource) == ToLowerCopy(manifestSource);
        }

        uint64_t NextMaterialRevision(uint64_t revision) noexcept
        {
            return revision == std::numeric_limits<uint64_t>::max()
                ? 1
                : revision + 1;
        }

        bool MaterialPropertyDeclarationEquivalent(
            const MaterialPropertyDecl& left,
            const MaterialPropertyDecl& right) noexcept
        {
            return left.Name == right.Name &&
                left.StableId == right.StableId &&
                left.DisplayName == right.DisplayName &&
                left.Category == right.Category &&
                left.Type == right.Type &&
                left.DefaultValue == right.DefaultValue &&
                left.DefaultInt == right.DefaultInt &&
                left.DefaultBool == right.DefaultBool &&
                left.bHasRange == right.bHasRange &&
                left.RangeMin == right.RangeMin &&
                left.RangeMax == right.RangeMax &&
                left.DefaultTextureToken == right.DefaultTextureToken &&
                left.TextureSemantic == right.TextureSemantic &&
                left.ColorSpace == right.ColorSpace &&
                left.TextureSlot == right.TextureSlot &&
                left.RegisterSpace == right.RegisterSpace &&
                left.ArrayCount == right.ArrayCount &&
                left.RowOffset == right.RowOffset &&
                left.ByteOffset == right.ByteOffset &&
                left.ByteSize == right.ByteSize;
        }

        bool MaterialRuntimePropertyEquivalent(
            const MaterialRuntimeProperty& left,
            const MaterialRuntimeProperty& right) noexcept
        {
            return MaterialPropertyDeclarationEquivalent(left.Decl, right.Decl) &&
                left.FloatValue == right.FloatValue &&
                left.ColorValue == right.ColorValue &&
                left.IntValue == right.IntValue &&
                left.BoolValue == right.BoolValue &&
                left.TextureValue == right.TextureValue &&
                left.Sampling.useAffine == right.Sampling.useAffine && left.Sampling.affineRow0 == right.Sampling.affineRow0 &&
                left.Sampling.affineRow1 == right.Sampling.affineRow1 && left.Sampling.texCoord == right.Sampling.texCoord && left.Sampling.offset == right.Sampling.offset &&
                left.Sampling.scale == right.Sampling.scale && left.Sampling.rotation == right.Sampling.rotation &&
                left.Sampling.wrapS == right.Sampling.wrapS && left.Sampling.wrapT == right.Sampling.wrapT &&
                left.Sampling.minFilter == right.Sampling.minFilter && left.Sampling.magFilter == right.Sampling.magFilter;
        }

        bool MaterialRuntimePropertiesEquivalent(
            const std::vector<MaterialRuntimeProperty>& left,
            const std::vector<MaterialRuntimeProperty>& right) noexcept
        {
            return left.size() == right.size() &&
                std::equal(
                    left.begin(),
                    left.end(),
                    right.begin(),
                    MaterialRuntimePropertyEquivalent);
        }

        bool ManifestRenderStateMatchesMaterial(
            const MaterialShaderManifest& manifest,
            const MaterialRes& material) noexcept
        {
            return material.Domain == ToString(manifest.Domain) &&
                material.ShadingModel == ToString(manifest.ShadingModel) &&
                material.BlendMode == ToString(manifest.BlendMode) &&
                material.CullMode == ToString(manifest.CullMode) &&
                material.DepthWrite == manifest.bDepthWrite;
        }

        [[nodiscard]] std::string FormatManifestFailure(
            std::span<const MaterialShaderDiagnostic> diagnostics,
            std::string_view fallback)
        {
            if (diagnostics.empty())
                return std::string(fallback);

            const MaterialShaderDiagnostic* firstError =
                FindFirstMaterialShaderError(diagnostics);
            if (!firstError || firstError == &diagnostics.front())
            {
                const std::string formatted =
                    FormatMaterialShaderDiagnostics(diagnostics);
                return formatted.empty() ? std::string(fallback) : formatted;
            }

            std::vector<MaterialShaderDiagnostic> errorFirst;
            errorFirst.reserve(diagnostics.size());
            errorFirst.push_back(*firstError);
            for (const MaterialShaderDiagnostic& diagnostic : diagnostics)
            {
                if (&diagnostic != firstError)
                    errorFirst.push_back(diagnostic);
            }
            const std::string formatted =
                FormatMaterialShaderDiagnostics(errorFirst);
            return formatted.empty() ? std::string(fallback) : formatted;
        }
    }

    Material::Material(std::string resPath)
        : mMatResPath(std::move(resPath))
    {
        mMatResPath = CanonicalizeMaterialResourcePath(mMatResPath);
        mSourceDocument = Utils::LoadJsonFile(ResolveMaterialLoadPath(mMatResPath));
        if (!mSourceDocument.is_object())
            throw std::invalid_argument("Material document must be a JSON object: " + mMatResPath);

        Serializer::Deserialize(mSourceDocument, mMatRes);
        mSchemaVersion = mSourceDocument.contains("SchemaVersion")
            ? mSourceDocument.value("SchemaVersion", 1)
            : 1;
        if (mSchemaVersion < 1 || mSchemaVersion > 2)
            throw std::invalid_argument("Unsupported material SchemaVersion: " + std::to_string(mSchemaVersion));
        mMatRes.SchemaVersion = mSchemaVersion;
        const auto parsedType = magic_enum::enum_cast<MaterialType>(mMatRes.Type);
        if (!parsedType)
            throw std::invalid_argument("Unsupported material Type: " + mMatRes.Type);
        mType = *parsedType;

        if (mSchemaVersion >= 2)
        {
            if (!mMatRes.VariantKeywords.empty())
            {
                throw std::invalid_argument(
                    "Material ABI v1 does not support VariantKeywords.");
            }
            if (mMatRes.Domain != "Surface" ||
                (mMatRes.ShadingModel != "Lit" && mMatRes.ShadingModel != "Unlit") ||
                (mMatRes.BlendMode != "Opaque" && mMatRes.BlendMode != "Mask" &&
                    mMatRes.BlendMode != "Transparent") ||
                (mMatRes.CullMode != "Back" && mMatRes.CullMode != "None") ||
                !mMatRes.Parameters.is_object() || !mMatRes.Textures.is_object() ||
                !mMatRes.OrphanedParameters.is_object() || !mMatRes.Properties.is_object() ||
                !std::isfinite(mMatRes.AlphaCutoff))
            {
                throw std::invalid_argument("Material V2 schema contains invalid render state or typed objects.");
            }
            const bool bExpectedDepthWrite = mMatRes.BlendMode != "Transparent";
            if (mMatRes.DepthWrite != bExpectedDepthWrite)
            {
                throw std::invalid_argument(
                    "Material ABI v1 requires Opaque/Mask depth write and Transparent no-write.");
            }
            // V2 render state is authoritative. Reject a conflicting typed
            // parameter instead of silently rendering with a different mask
            // cutoff than the imported material claims to use.
            const auto cutoffParameter = mMatRes.Parameters.find("g_AlphaCutoff");
            if (cutoffParameter != mMatRes.Parameters.end() &&
                cutoffParameter->is_object() && cutoffParameter->contains("Value"))
            {
                const Json& value = (*cutoffParameter)["Value"];
                if (!value.is_number() ||
                    std::abs(value.get<float>() - mMatRes.AlphaCutoff) > 1e-4f)
                {
                    throw std::invalid_argument(
                        "Material V2 g_AlphaCutoff disagrees with authoritative AlphaCutoff: " +
                        mMatResPath);
                }
            }
        }

        if (!mMatRes.PassName.empty())
            mPassName = mMatRes.PassName;

        if (!mMatRes.ShaderPath.empty())
        {
            if (!TryCanonicalizeShaderPath(mMatRes.ShaderPath, mShaderPath))
            {
                throw std::invalid_argument(
                    "Material ShaderPath must be a safe Shaders-relative .hlsl path: " +
                    mMatRes.ShaderPath);
            }
            mPassName = ShaderPathToPassName(mShaderPath);
        }
        else if (!mPassName.empty())
        {
            const std::string legacyShaderPath = "Shaders/" + mPassName + ".hlsl";
            if (!TryCanonicalizeShaderPath(legacyShaderPath, mShaderPath))
                throw std::invalid_argument("Legacy material PassName is not a safe shader identity.");
        }

        if (mPassName.empty())
            mPassName = "BasePBR";

        if (mSchemaVersion >= 2 && mType == MaterialType::Default &&
            ToLowerCopy(mShaderPath) != "shaders/basepbr.hlsl")
        {
            throw std::invalid_argument(
                "Material V2 Type=Default ignores custom ShaderPath; set Type=Custom to render the declared shader.");
        }

        Serializer::Deserialize(mMatRes.Properties, mPbrMatRes);
        const auto validateLegacyTexturePath = [&](std::string& path)
        {
            if (path.empty())
                return;
            if (IsBuiltinTextureToken(path))
            {
                path = ToLowerCopy(path);
                return;
            }
            std::string normalized;
            if (!Utils::TryNormalizeProjectRelativePath(path, normalized) ||
                !ToLowerCopy(normalized).starts_with("assets/") ||
                Utils::Utf8ToPath(normalized).extension().empty())
            {
                throw std::invalid_argument(
                    "Material contains an unsafe legacy texture path: " + path);
            }
            path = std::move(normalized);
        };
        validateLegacyTexturePath(
            mPbrMatRes.pbrMetallicRoughness.baseColorTexture.textureRes.textureSourcePath);
        validateLegacyTexturePath(
            mPbrMatRes.normalTexture.textureRes.textureSourcePath);
        validateLegacyTexturePath(
            mPbrMatRes.pbrMetallicRoughness.metallicRoughnessTexture.textureRes.textureSourcePath);
        validateLegacyTexturePath(
            mPbrMatRes.occlusionTexture.textureRes.textureSourcePath);
        validateLegacyTexturePath(
            mPbrMatRes.emissiveTexture.textureRes.textureSourcePath);

        const bool bHasLegacyDefaultPbrFactors =
            glm::epsilonEqual(mPbrMatRes.pbrMetallicRoughness.metallicFactor, 0.1f, 1e-4f) &&
            glm::epsilonEqual(mPbrMatRes.pbrMetallicRoughness.roughnessFactor, 0.9f, 1e-4f);
        if (mSchemaVersion == 1 &&
            bHasLegacyDefaultPbrFactors &&
            mPbrMatRes.bUseMetallicRoughnessMap)
        {
            mPbrMatRes.pbrMetallicRoughness.metallicFactor = 1.0f;
            mPbrMatRes.pbrMetallicRoughness.roughnessFactor = 1.0f;
        }

        if (mSchemaVersion >= 2)
        {
            const std::string blendMode = ToLowerCopy(mMatRes.BlendMode);
            if (blendMode == "transparent")
                mPbrMatRes.alphaMode = "BLEND";
            else if (blendMode == "mask")
                mPbrMatRes.alphaMode = "MASK";
            else
                mPbrMatRes.alphaMode = "OPAQUE";
            mPbrMatRes.alphaCutoff = std::clamp(mMatRes.AlphaCutoff, 0.0f, 1.0f);
            mPbrMatRes.doubleSided = ToLowerCopy(mMatRes.CullMode) == "none";
            if (mMatRes.OrphanedParameters.is_object())
                mOrphanedParameters = mMatRes.OrphanedParameters;
        }

        baseColorFactor = mPbrMatRes.pbrMetallicRoughness.baseColorFactor;
        bUseAlbedoMap = mPbrMatRes.bUseAlbedoMap;
        roughnessFactor = mPbrMatRes.pbrMetallicRoughness.roughnessFactor;
        metallicFactor = mPbrMatRes.pbrMetallicRoughness.metallicFactor;
        bUseMetallicRoughnessMap = mPbrMatRes.bUseMetallicRoughnessMap;
        bUseNormalMap = mPbrMatRes.bUseNormalMap;
        aoStrength = mPbrMatRes.occlusionTexture.strength;
        bUseAoMap = mPbrMatRes.bUseAoMap;
        emissiveFactor = mPbrMatRes.emissiveFactor;
        bUseEmissiveMap = mPbrMatRes.bUseEmissiveMap;

        BuildExposedPropertiesFromShader();
        SyncExposedPropertiesFromLegacyPbr();
        LoadExposedPropertiesFromMaterialJson(mMatRes.Properties);
        if (mSchemaVersion >= 2)
            LoadTypedPropertiesFromMaterialJson(mSourceDocument);
        SyncLegacyPbrFromExposedProperties();
        SyncRenderStateToExposedProperties();
    }

    std::string Material::GetResourcePath() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mMatResPath;
    }

    std::string Material::GetShaderPath() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mShaderPath;
    }

    int Material::GetSchemaVersion() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mSchemaVersion;
    }

    uint64_t Material::GetRuntimeRevision() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mRuntimeRevision;
    }

    std::string Material::GetExpectedRecursiveSourceHash() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mExpectedRecursiveSourceHash;
    }

    std::string Material::GetExpectedBindingSignature() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mExpectedBindingSignature;
    }

    uint32_t Material::GetExpectedMaterialAbiVersion() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mExpectedMaterialAbiVersion;
    }

    std::string Material::GetExpectedArtifactIdentityHash() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mExpectedArtifactIdentityHash;
    }

    uint64_t Material::GetExpectedProgramGeneration() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mExpectedProgramGeneration;
    }

    Json Material::GetOrphanedParameters() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mOrphanedParameters;
    }

    bool Material::HasExposedProperties() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return bUseShaderProperties;
    }

    bool Material::IsDoubleSided() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mPbrMatRes.doubleSided;
    }

#if WITH_EDITOR
    bool Material::SetShaderPath(const std::string& shaderPath)
    {
        if (shaderPath.empty())
            return false;

        std::string candidate = shaderPath;
        try
        {
            const std::filesystem::path candidatePath = Utils::Utf8ToPath(candidate);
            if (candidatePath.is_absolute() || candidatePath.has_root_name())
                candidate = ConfigManager::GetRelativePath(candidatePath);
        }
        catch (...)
        {
            return false;
        }

        std::string normalized;
        if (!TryCanonicalizeShaderPath(candidate, normalized))
            return false;

        std::optional<MaterialShaderManifest> publishedManifest;
        if (ToLowerCopy(normalized) != "shaders/basepbr.hlsl")
        {
            MaterialShaderManifestLoadResult manifestResult =
                LoadPublishedMaterialManifest(normalized);
            if (!manifestResult.Succeeded() ||
                !ManifestMatchesShaderPath(manifestResult.Manifest, normalized))
                return false;
            publishedManifest = std::move(manifestResult.Manifest);
        }

        try
        {
            Material candidate;
            candidate.CopyRuntimeStateFrom(*this);
            const std::vector<MaterialRuntimeProperty> previousProperties =
                candidate.mShaderProperties;
            const Json previousOrphans = candidate.mOrphanedParameters;

            candidate.mShaderPath = normalized;
            candidate.mPassName = ShaderPathToPassName(normalized);
            candidate.mType = candidate.mPassName == "BasePBR"
                ? MaterialType::Default
                : MaterialType::Custom;
            if (publishedManifest)
            {
                candidate.mMatRes.Domain = ToString(publishedManifest->Domain);
                candidate.mMatRes.ShadingModel = ToString(publishedManifest->ShadingModel);
                candidate.mMatRes.BlendMode = ToString(publishedManifest->BlendMode);
                candidate.mMatRes.CullMode = ToString(publishedManifest->CullMode);
                candidate.mMatRes.DepthWrite = publishedManifest->bDepthWrite;
                candidate.mPbrMatRes.alphaMode = publishedManifest->BlendMode ==
                        MaterialShaderBlendMode::Transparent
                    ? "BLEND"
                    : publishedManifest->BlendMode == MaterialShaderBlendMode::Mask
                        ? "MASK"
                        : "OPAQUE";
                candidate.mPbrMatRes.doubleSided =
                    publishedManifest->CullMode == MaterialShaderCullMode::None;
                candidate.mShaderProperties = BuildRuntimeProperties(*publishedManifest);
                candidate.bUseShaderProperties = !candidate.mShaderProperties.empty();
                candidate.mExpectedRecursiveSourceHash =
                    publishedManifest->RecursiveSourceHash;
                candidate.mExpectedBindingSignature =
                    publishedManifest->BindingSignatureHash;
                candidate.mExpectedMaterialAbiVersion =
                    publishedManifest->MaterialAbiVersion;
                candidate.mExpectedArtifactIdentityHash =
                    BuildMaterialShaderArtifactIdentityHash(*publishedManifest);
                candidate.mExpectedProgramGeneration = 0;
            }
            else
            {
                candidate.BuildExposedPropertiesFromShader();
            }
            candidate.SyncExposedPropertiesFromLegacyPbr();
            candidate.MigrateRuntimeProperties(previousProperties, previousOrphans);
            candidate.SyncLegacyPbrFromExposedProperties();
            candidate.SyncRenderStateToExposedProperties();
            candidate.mRuntimeRevision = NextMaterialRevision(candidate.mRuntimeRevision);
            CopyRuntimeStateFrom(candidate);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void Material::SaveToRes()
    {
        const std::string materialPath =
            CanonicalizeMaterialResourcePath(GetResourcePath());
        const std::filesystem::path savePath =
            ResolveMaterialSavePath(materialPath);
        Json materialJson = BuildSerializedDocument();
        // Disk IO deliberately happens without the runtime-state lock. The
        // document above is an immutable snapshot of the draft revision.
        Utils::SaveJsonToFile(materialJson, savePath);
        std::scoped_lock lock(mRuntimeMutex);
        mSourceDocument = std::move(materialJson);
    }
#endif

    Json Material::BuildSerializedDocument()
    {
        std::scoped_lock lock(mRuntimeMutex);
        ApplyRuntimePropertyChanges();
        Json propertiesJson = mMatRes.Properties;
        mPbrMatRes.bUseAlbedoMap = bUseAlbedoMap;
        mPbrMatRes.pbrMetallicRoughness.baseColorFactor = baseColorFactor;
        mPbrMatRes.bUseAoMap = bUseAoMap;
        mPbrMatRes.occlusionTexture.strength = aoStrength;
        mPbrMatRes.bUseEmissiveMap = bUseEmissiveMap;
        mPbrMatRes.emissiveFactor = emissiveFactor;
        mPbrMatRes.bUseMetallicRoughnessMap = bUseMetallicRoughnessMap;
        mPbrMatRes.pbrMetallicRoughness.metallicFactor = metallicFactor;
        mPbrMatRes.pbrMetallicRoughness.roughnessFactor = roughnessFactor;
        mPbrMatRes.bUseNormalMap = bUseNormalMap;
        Serializer::Serialize(propertiesJson, mPbrMatRes);

        if (bUseShaderProperties)
        {
            Json exposedJson;
            for (const MaterialRuntimeProperty& property : mShaderProperties)
            {
                switch (property.Decl.Type)
                {
                case MaterialPropertyType::Float:
                    exposedJson[property.Decl.Name] = property.FloatValue;
                    break;
                case MaterialPropertyType::Float2:
                    exposedJson[property.Decl.Name] = {
                        property.ColorValue.x,
                        property.ColorValue.y };
                    break;
                case MaterialPropertyType::Float3:
                    exposedJson[property.Decl.Name] = {
                        property.ColorValue.x,
                        property.ColorValue.y,
                        property.ColorValue.z };
                    break;
                case MaterialPropertyType::Color:
                    exposedJson[property.Decl.Name] = {
                        property.ColorValue.x,
                        property.ColorValue.y,
                        property.ColorValue.z,
                        property.ColorValue.w };
                    break;
                case MaterialPropertyType::Texture:
                {
                    std::string resolvedPath = property.TextureValue;
                    if (resolvedPath.empty())
                        resolvedPath = ResolveTexturePathFromToken(property.Decl.DefaultTextureToken);
                    if (resolvedPath.empty())
                        resolvedPath = property.Decl.DefaultTextureToken;
                    exposedJson[property.Decl.Name] = resolvedPath;
                    break;
                }
                case MaterialPropertyType::Bool:
                    exposedJson[property.Decl.Name] = property.BoolValue;
                    break;
                case MaterialPropertyType::Int:
                    exposedJson[property.Decl.Name] = property.IntValue;
                    break;
                }
            }
            propertiesJson["ExposedProperties"] = std::move(exposedJson);
        }

        mSchemaVersion = 2;
        mMatRes.SchemaVersion = mSchemaVersion;
        mMatRes.Type = Utils::EnumName(mType);
        mMatRes.PassName = mPassName;
        mMatRes.ShaderPath = mShaderPath;
        mMatRes.BlendMode = mPbrMatRes.alphaMode == "BLEND"
            ? "Transparent"
            : mPbrMatRes.alphaMode == "MASK" ? "Mask" : "Opaque";
        mMatRes.CullMode = mPbrMatRes.doubleSided ? "None" : "Back";
        mMatRes.DepthWrite = mPbrMatRes.alphaMode != "BLEND";
        mMatRes.AlphaCutoff = std::clamp(mPbrMatRes.alphaCutoff, 0.0f, 1.0f);
        mMatRes.Parameters = SerializeTypedParameters();
        mMatRes.Textures = SerializeTypedTextures();
        mMatRes.OrphanedParameters = mOrphanedParameters;
        mMatRes.Properties = std::move(propertiesJson);

        Json materialJson = mSourceDocument.is_object()
            ? mSourceDocument
            : Json::object();
        Json knownFields;
        Serializer::Serialize(knownFields, mMatRes);
        for (auto iterator = knownFields.begin(); iterator != knownFields.end(); ++iterator)
            materialJson[iterator.key()] = iterator.value();
        return materialJson;
    }

    MaterialTextureReference Material::GetTextureReference(MaterialTextureSemantic semantic) const
    {
        std::scoped_lock lock(mRuntimeMutex);
        MaterialTextureReference reference;
        switch (semantic)
        {
        case MaterialTextureSemantic::BaseColor:
            reference.AssetPath = mPbrMatRes.pbrMetallicRoughness.baseColorTexture.textureRes.textureSourcePath;
            reference.Sampling = mPbrMatRes.pbrMetallicRoughness.baseColorTexture.sampling;
            reference.Sampling.texCoord = mPbrMatRes.pbrMetallicRoughness.baseColorTexture.texCoord;
            reference.Fallback = "white";
            reference.bEnabled = bUseAlbedoMap;
            break;
        case MaterialTextureSemantic::Normal:
            reference.AssetPath = mPbrMatRes.normalTexture.textureRes.textureSourcePath;
            reference.Sampling = mPbrMatRes.normalTexture.sampling;
            reference.Sampling.texCoord = mPbrMatRes.normalTexture.texCoord;
            reference.Fallback = "normal";
            reference.bEnabled = bUseNormalMap;
            break;
        case MaterialTextureSemantic::MetallicRoughness:
            reference.AssetPath = mPbrMatRes.pbrMetallicRoughness.metallicRoughnessTexture.textureRes.textureSourcePath;
            reference.Sampling = mPbrMatRes.pbrMetallicRoughness.metallicRoughnessTexture.sampling;
            reference.Sampling.texCoord = mPbrMatRes.pbrMetallicRoughness.metallicRoughnessTexture.texCoord;
            reference.Fallback = "white";
            reference.bEnabled = bUseMetallicRoughnessMap;
            break;
        case MaterialTextureSemantic::Occlusion:
            reference.AssetPath = mPbrMatRes.occlusionTexture.textureRes.textureSourcePath;
            reference.Sampling = mPbrMatRes.occlusionTexture.sampling;
            reference.Sampling.texCoord = mPbrMatRes.occlusionTexture.texCoord;
            reference.Fallback = "white";
            reference.bEnabled = bUseAoMap;
            break;
        case MaterialTextureSemantic::Emissive:
            reference.AssetPath = mPbrMatRes.emissiveTexture.textureRes.textureSourcePath;
            reference.Sampling = mPbrMatRes.emissiveTexture.sampling;
            reference.Sampling.texCoord = mPbrMatRes.emissiveTexture.texCoord;
            reference.Fallback = "white";
            reference.bEnabled = bUseEmissiveMap;
            break;
        }

        reference.AssetPath = CollapseBuiltinTexturePath(reference.AssetPath);
        return reference;
    }

    bool Material::IsCustomSurfaceShader() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        return mType != MaterialType::Default &&
            mType != MaterialType::PostProcess &&
            ToLowerCopy(mPassName) != "basepbr" &&
            ToLowerCopy(mShaderPath) != "shaders/basepbr.hlsl";
    }

    std::vector<MaterialParameterValue> Material::BuildParameterValues() const
    {
        std::scoped_lock lock(mRuntimeMutex);
        std::vector<MaterialParameterValue> values;
        values.reserve(mShaderProperties.size());
        for (const MaterialRuntimeProperty& property : mShaderProperties)
        {
            MaterialParameterValue value;
            value.StableId = property.Decl.StableId;
            value.Name = property.Decl.Name;
            value.Numeric = {
                property.ColorValue.x,
                property.ColorValue.y,
                property.ColorValue.z,
                property.ColorValue.w };
            value.Boolean = property.BoolValue;
            value.Integer = property.IntValue;
            value.TexturePath = property.TextureValue;
            value.TextureUvSet = property.Sampling.texCoord;
            switch (property.Decl.Type)
            {
            case MaterialPropertyType::Texture:
                value.Type = MaterialShaderPropertyType::Texture2D;
                break;
            case MaterialPropertyType::Float:
                value.Type = MaterialShaderPropertyType::Float;
                value.Numeric[0] = property.FloatValue;
                break;
            case MaterialPropertyType::Color:
                value.Type = MaterialShaderPropertyType::Color;
                break;
            case MaterialPropertyType::Float2:
                value.Type = MaterialShaderPropertyType::Float2;
                break;
            case MaterialPropertyType::Float3:
                value.Type = MaterialShaderPropertyType::Float3;
                break;
            case MaterialPropertyType::Bool:
                value.Type = MaterialShaderPropertyType::Bool;
                break;
            case MaterialPropertyType::Int:
                value.Type = MaterialShaderPropertyType::Int;
                break;
            }
            values.push_back(std::move(value));
        }
        return values;
    }

    RenderMeshSection::MaterialInstanceSnapshot Material::BuildRenderSnapshot() const
    {
        // One lock covers identity, layout, packed values, textures and render
        // state so the renderer never observes a half-migrated refresh.
        std::scoped_lock lock(mRuntimeMutex);
        RenderMeshSection::MaterialInstanceSnapshot snapshot;
        snapshot.AssetPath = mMatResPath;
        snapshot.ShaderPath = GetShaderPath();
        snapshot.Revision = GetRuntimeRevision();
        snapshot.ExpectedRecursiveSourceHash = mExpectedRecursiveSourceHash;
        snapshot.ExpectedBindingSignature = mExpectedBindingSignature;
        snapshot.ExpectedMaterialAbiVersion = mExpectedMaterialAbiVersion;
        snapshot.ExpectedArtifactIdentityHash = mExpectedArtifactIdentityHash;
        snapshot.ExpectedProgramGeneration = mExpectedProgramGeneration;
        snapshot.ParameterValues = BuildParameterValues();
        snapshot.BaseColor = baseColorFactor;
        snapshot.Emissive = emissiveFactor;
        snapshot.Metallic = metallicFactor;
        snapshot.Roughness = roughnessFactor;
        snapshot.AoStrength = aoStrength;
        snapshot.NormalScale = mPbrMatRes.normalTexture.scale;
        constexpr std::array<MaterialTextureSemantic, 5> semantics = {
            MaterialTextureSemantic::BaseColor,
            MaterialTextureSemantic::Normal,
            MaterialTextureSemantic::MetallicRoughness,
            MaterialTextureSemantic::Occlusion,
            MaterialTextureSemantic::Emissive,
        };
        for (size_t index = 0; index < semantics.size(); ++index)
        {
            const MaterialTextureReference reference = GetTextureReference(semantics[index]);
            snapshot.LegacyTextures[index] = {
                .AssetPath = reference.AssetPath,
                .Sampling = reference.Sampling,
                .Fallback = reference.Fallback,
                .bEnabled = reference.bEnabled,
            };
        }
        snapshot.AlphaMode = mPbrMatRes.alphaMode == "BLEND"
            ? RenderMaterialAlphaMode::Blend
            : mPbrMatRes.alphaMode == "MASK"
                ? RenderMaterialAlphaMode::Mask
                : RenderMaterialAlphaMode::Opaque;
        snapshot.AlphaCutoff = std::isfinite(mPbrMatRes.alphaCutoff)
            ? std::clamp(mPbrMatRes.alphaCutoff, 0.0f, 1.0f)
            : 0.5f;
        snapshot.bDoubleSided = mPbrMatRes.doubleSided;
        snapshot.bDepthWrite = mMatRes.DepthWrite;
        snapshot.bCustomSurface = IsCustomSurfaceShader();
        snapshot.bValid = true;
        return snapshot;
    }

    void Material::SetTexturePath(
        MaterialTextureSemantic semantic,
        std::string assetPath,
        bool bEnabled)
    {
        std::scoped_lock lock(mRuntimeMutex);
        if (!assetPath.empty() && !IsBuiltinTextureToken(assetPath))
        {
            std::string normalized;
            if (!Utils::TryNormalizeProjectRelativePath(assetPath, normalized) ||
                !ToLowerCopy(normalized).starts_with("assets/") ||
                Utils::Utf8ToPath(normalized).extension().empty())
            {
                HE_CORE_WARN("Rejected unsafe material texture path: {0}", assetPath);
                return;
            }
            assetPath = std::move(normalized);
        }
        else
        {
            assetPath = ToLowerCopy(assetPath);
        }
        switch (semantic)
        {
        case MaterialTextureSemantic::BaseColor:
            mPbrMatRes.pbrMetallicRoughness.baseColorTexture.textureRes.textureSourcePath = assetPath;
            bUseAlbedoMap = bEnabled;
            mPbrMatRes.bUseAlbedoMap = bEnabled;
            break;
        case MaterialTextureSemantic::Normal:
            mPbrMatRes.normalTexture.textureRes.textureSourcePath = assetPath;
            bUseNormalMap = bEnabled;
            mPbrMatRes.bUseNormalMap = bEnabled;
            break;
        case MaterialTextureSemantic::MetallicRoughness:
            mPbrMatRes.pbrMetallicRoughness.metallicRoughnessTexture.textureRes.textureSourcePath = assetPath;
            bUseMetallicRoughnessMap = bEnabled;
            mPbrMatRes.bUseMetallicRoughnessMap = bEnabled;
            break;
        case MaterialTextureSemantic::Occlusion:
            mPbrMatRes.occlusionTexture.textureRes.textureSourcePath = assetPath;
            bUseAoMap = bEnabled;
            mPbrMatRes.bUseAoMap = bEnabled;
            break;
        case MaterialTextureSemantic::Emissive:
            mPbrMatRes.emissiveTexture.textureRes.textureSourcePath = assetPath;
            bUseEmissiveMap = bEnabled;
            mPbrMatRes.bUseEmissiveMap = bEnabled;
            break;
        }

        for (MaterialRuntimeProperty& property : mShaderProperties)
        {
            if (property.Decl.Type == MaterialPropertyType::Texture && MatchesSemantic(semantic, property))
            {
                property.TextureValue = CollapseBuiltinTexturePath(assetPath);
                break;
            }
        }
        if (mRuntimeRevision == std::numeric_limits<uint64_t>::max())
            mRuntimeRevision = 1;
        else
            ++mRuntimeRevision;
    }

    void Material::ApplyRuntimePropertyChanges()
    {
        std::scoped_lock lock(mRuntimeMutex);
        SyncLegacyPbrFromExposedProperties();
        if (mSchemaVersion >= 2)
        {
            mMatRes.BlendMode = mPbrMatRes.alphaMode == "BLEND"
                ? "Transparent"
                : mPbrMatRes.alphaMode == "MASK" ? "Mask" : "Opaque";
            mMatRes.CullMode = mPbrMatRes.doubleSided ? "None" : "Back";
            mMatRes.DepthWrite = mPbrMatRes.alphaMode != "BLEND";
            mMatRes.AlphaCutoff = std::clamp(mPbrMatRes.alphaCutoff, 0.0f, 1.0f);
            SyncRenderStateToExposedProperties();
        }
        if (mRuntimeRevision == std::numeric_limits<uint64_t>::max())
            mRuntimeRevision = 1;
        else
            ++mRuntimeRevision;
    }

    bool Material::RefreshShaderLayoutFromManifest(std::string* error)
    {
        if (error)
            error->clear();
        std::string shaderPath;
        {
            std::scoped_lock lock(mRuntimeMutex);
            if (!IsCustomSurfaceShader())
            {
                if (error)
                    *error = "Only custom surface materials have a published material manifest.";
                return false;
            }
            shaderPath = mShaderPath;
        }

        // Manifest IO is intentionally outside the material state lock.
        MaterialShaderManifestLoadResult manifestResult =
            LoadPublishedMaterialManifest(shaderPath);
        if (!manifestResult.Succeeded())
        {
            if (error)
            {
                *error = FormatManifestFailure(
                    manifestResult.Diagnostics,
                    "The published material manifest is unavailable.");
            }
            return false;
        }
        if (!ManifestMatchesShaderPath(manifestResult.Manifest, shaderPath))
        {
            if (error)
                *error = "The published manifest source identity does not match this material shader.";
            return false;
        }

        const MaterialShaderManifest& manifest = manifestResult.Manifest;
        const std::string artifactIdentity =
            BuildMaterialShaderArtifactIdentityHash(manifest);
        MaterialShaderProgramIdentity identity{
            .SourcePath = shaderPath,
            .RecursiveContentHash = manifest.RecursiveSourceHash,
            .MaterialAbiVersion = manifest.MaterialAbiVersion,
            .BindingSignatureHash = manifest.BindingSignatureHash,
            .ArtifactIdentityHash = artifactIdentity,
        };
        uint64_t generation = 0;
        {
            std::scoped_lock lock(mRuntimeMutex);
            if (Utils::MakeProjectPathCacheKey(mShaderPath) !=
                Utils::MakeProjectPathCacheKey(shaderPath))
            {
                if (error)
                    *error = "The material shader changed while its manifest was loading.";
                return false;
            }
            const bool bProgramChanged =
                mExpectedRecursiveSourceHash != identity.RecursiveContentHash ||
                mExpectedBindingSignature != identity.BindingSignatureHash ||
                mExpectedMaterialAbiVersion != identity.MaterialAbiVersion ||
                mExpectedArtifactIdentityHash != identity.ArtifactIdentityHash;
            generation = mExpectedProgramGeneration == 0
                ? 1
                : bProgramChanged
                    ? NextMaterialRevision(mExpectedProgramGeneration)
                    : mExpectedProgramGeneration;
        }
        MaterialShaderLayoutRefreshResult refresh =
            RefreshShaderLayoutFromPublishedProgram(manifest, identity, generation);
        if (!refresh.bSucceeded && error)
            *error = std::move(refresh.Error);
        return refresh.bSucceeded;
    }

    MaterialShaderLayoutRefreshResult Material::RefreshShaderLayoutFromPublishedProgram(
        const MaterialShaderManifest& manifest,
        const MaterialShaderProgramIdentity& identity,
        uint64_t generation)
    {
        // All validation after external publication and the final state copy
        // are CPU-only and execute as one material transaction.
        std::scoped_lock lock(mRuntimeMutex);
        MaterialShaderLayoutRefreshResult result;
        if (!IsCustomSurfaceShader())
        {
            result.Error = "Only custom surface materials have a published material manifest.";
            return result;
        }
        if (!ManifestMatchesShaderPath(manifest, mShaderPath) ||
            Utils::MakeProjectPathCacheKey(identity.SourcePath) !=
                Utils::MakeProjectPathCacheKey(mShaderPath))
        {
            result.Error =
                "The published program source identity does not match this material shader.";
            return result;
        }

        const std::string expectedArtifactIdentity =
            BuildMaterialShaderArtifactIdentityHash(manifest);
        if (generation == 0 ||
            identity.RecursiveContentHash != manifest.RecursiveSourceHash ||
            identity.MaterialAbiVersion != manifest.MaterialAbiVersion ||
            identity.BindingSignatureHash != manifest.BindingSignatureHash ||
            identity.ArtifactIdentityHash != expectedArtifactIdentity)
        {
            result.Error =
                "The published program identity does not match its material manifest.";
            return result;
        }

        try
        {
            Material candidate;
            candidate.CopyRuntimeStateFrom(*this);
            const std::vector<MaterialRuntimeProperty> previousProperties =
                candidate.mShaderProperties;
            const Json previousOrphans = candidate.mOrphanedParameters;

            candidate.mShaderProperties = BuildRuntimeProperties(manifest);
            candidate.bUseShaderProperties = !candidate.mShaderProperties.empty();
            candidate.MigrateRuntimeProperties(previousProperties, previousOrphans);

            candidate.mMatRes.Domain = ToString(manifest.Domain);
            candidate.mMatRes.ShadingModel = ToString(manifest.ShadingModel);
            candidate.mMatRes.BlendMode = ToString(manifest.BlendMode);
            candidate.mMatRes.CullMode = ToString(manifest.CullMode);
            candidate.mMatRes.DepthWrite = manifest.bDepthWrite;
            candidate.mPbrMatRes.alphaMode = manifest.BlendMode ==
                    MaterialShaderBlendMode::Transparent
                ? "BLEND"
                : manifest.BlendMode == MaterialShaderBlendMode::Mask
                    ? "MASK"
                    : "OPAQUE";
            candidate.mPbrMatRes.doubleSided =
                manifest.CullMode == MaterialShaderCullMode::None;
            candidate.mExpectedRecursiveSourceHash = identity.RecursiveContentHash;
            candidate.mExpectedBindingSignature = identity.BindingSignatureHash;
            candidate.mExpectedMaterialAbiVersion = identity.MaterialAbiVersion;
            candidate.mExpectedArtifactIdentityHash = identity.ArtifactIdentityHash;
            candidate.mExpectedProgramGeneration = generation;

            candidate.SyncLegacyPbrFromExposedProperties();
            candidate.SyncRenderStateToExposedProperties();
            result.bPersistentStateChanged =
                !MaterialRuntimePropertiesEquivalent(
                    mShaderProperties,
                    candidate.mShaderProperties) ||
                mOrphanedParameters != candidate.mOrphanedParameters ||
                !ManifestRenderStateMatchesMaterial(manifest, mMatRes);
            candidate.mRuntimeRevision = NextMaterialRevision(candidate.mRuntimeRevision);
            CopyRuntimeStateFrom(candidate);
            result.bSucceeded = true;
            return result;
        }
        catch (const std::exception& exception)
        {
            result.Error = exception.what();
            return result;
        }
        catch (...)
        {
            result.Error =
                "Material shader layout migration failed with an unknown error.";
            return result;
        }
    }

    void Material::CopyRuntimeStateFrom(const Material& other)
    {
        if (this == &other)
            return;

        std::scoped_lock lock(mRuntimeMutex, other.mRuntimeMutex);
        CopyRuntimeStateFromUnlocked(other);
    }

    void Material::PublishRuntimeStateFrom(const Material& other)
    {
        if (this == &other)
            return;

        std::scoped_lock lock(mRuntimeMutex, other.mRuntimeMutex);
        const uint64_t previousRevision = mRuntimeRevision;
        CopyRuntimeStateFromUnlocked(other);
        mRuntimeRevision = NextMaterialRevision(previousRevision);
    }

    void Material::CopyRuntimeStateFromUnlocked(const Material& other)
    {
        mType = other.mType;
        mPassName = other.mPassName;
        mShaderPath = other.mShaderPath;
        mMatRes = other.mMatRes;
        mPbrMatRes = other.mPbrMatRes;
        bUseAlbedoMap = other.bUseAlbedoMap;
        baseColorFactor = other.baseColorFactor;
        bUseNormalMap = other.bUseNormalMap;
        bUseMetallicRoughnessMap = other.bUseMetallicRoughnessMap;
        metallicFactor = other.metallicFactor;
        roughnessFactor = other.roughnessFactor;
        bUseAoMap = other.bUseAoMap;
        aoStrength = other.aoStrength;
        bUseEmissiveMap = other.bUseEmissiveMap;
        emissiveFactor = other.emissiveFactor;
        bUseShaderProperties = other.bUseShaderProperties;
        mShaderProperties = other.mShaderProperties;
        mSchemaVersion = other.mSchemaVersion;
        mSourceDocument = other.mSourceDocument;
        mOrphanedParameters = other.mOrphanedParameters;
        mRuntimeRevision = other.mRuntimeRevision;
        mExpectedRecursiveSourceHash = other.mExpectedRecursiveSourceHash;
        mExpectedBindingSignature = other.mExpectedBindingSignature;
        mExpectedMaterialAbiVersion = other.mExpectedMaterialAbiVersion;
        mExpectedArtifactIdentityHash = other.mExpectedArtifactIdentityHash;
        mExpectedProgramGeneration = other.mExpectedProgramGeneration;
    }

    std::string Material::ResolveTexturePathFromToken(const std::string& token) const
    {
        if (token.empty())
            return {};

        const std::string lower = ToLowerCopy(token);
        if (lower == "white")
            return "Assets\\Textures\\white.png";
        if (lower == "black")
            return "Assets\\Textures\\black.png";
        if (lower == "normal")
            return "Assets\\Textures\\DefaultNormal.png";

        const bool bLooksLikePath = token.find('\\') != std::string::npos ||
            token.find('/') != std::string::npos || token.find('.') != std::string::npos;
        return bLooksLikePath ? NormalizePath(token) : std::string{};
    }

    void Material::ResetTexturePropertyToDefault(MaterialRuntimeProperty& property)
    {
        std::scoped_lock lock(mRuntimeMutex);
        if (property.Decl.Type != MaterialPropertyType::Texture)
            return;

        std::string defaultPath = IsBuiltinTextureToken(property.Decl.DefaultTextureToken)
            ? ToLowerCopy(property.Decl.DefaultTextureToken)
            : ResolveTexturePathFromToken(property.Decl.DefaultTextureToken);
        if (defaultPath.empty())
            defaultPath = property.Decl.DefaultTextureToken;
        property.TextureValue = std::move(defaultPath);
    }

    void Material::BuildExposedPropertiesFromShader()
    {
        mShaderProperties.clear();
        mExpectedRecursiveSourceHash.clear();
        mExpectedBindingSignature.clear();
        mExpectedMaterialAbiVersion = 0;
        mExpectedArtifactIdentityHash.clear();
        mExpectedProgramGeneration = 0;
        std::vector<MaterialPropertyDecl> declarations;
        const bool bCustomDefinition =
            ToLowerCopy(mShaderPath) != "shaders/basepbr.hlsl";
        if (bCustomDefinition)
        {
            MaterialShaderManifestLoadResult manifestResult =
                LoadPublishedMaterialManifest(mShaderPath);
            if (manifestResult.Succeeded())
            {
                if (!ManifestMatchesShaderPath(manifestResult.Manifest, mShaderPath))
                    throw std::runtime_error(
                        "Published manifest source identity does not match the material shader path.");
                if (mSchemaVersion >= 2 && IsCustomSurfaceShader() &&
                    !ManifestRenderStateMatchesMaterial(manifestResult.Manifest, mMatRes))
                {
                    throw std::invalid_argument(
                        "Material render state does not match its published shader manifest.");
                }
                mShaderProperties = BuildRuntimeProperties(manifestResult.Manifest);
                bUseShaderProperties = !mShaderProperties.empty();
                mExpectedRecursiveSourceHash =
                    manifestResult.Manifest.RecursiveSourceHash;
                mExpectedBindingSignature =
                    manifestResult.Manifest.BindingSignatureHash;
                mExpectedMaterialAbiVersion =
                    manifestResult.Manifest.MaterialAbiVersion;
                mExpectedArtifactIdentityHash =
                    BuildMaterialShaderArtifactIdentityHash(manifestResult.Manifest);
                mExpectedProgramGeneration = 0;
                return;
            }
            else if (mSchemaVersion >= 2 &&
                (mType == MaterialType::Custom || mType == MaterialType::CustomPBR))
            {
                const std::string reason = FormatManifestFailure(
                    manifestResult.Diagnostics,
                    "manifest is unavailable");
                throw std::runtime_error(
                    "V2 custom material requires a valid published shader manifest: " + reason);
            }
        }
        if (declarations.empty())
            declarations = ShaderPropertyParser::Parse(mShaderPath);
        bUseShaderProperties = !declarations.empty();
        if (!bUseShaderProperties)
            return;

        mShaderProperties.reserve(declarations.size());
        for (const MaterialPropertyDecl& declaration : declarations)
        {
            MaterialRuntimeProperty property;
            property.Decl = declaration;
            property.FloatValue = declaration.DefaultValue.x;
            property.ColorValue = declaration.DefaultValue;
            property.IntValue = declaration.DefaultInt;
            property.BoolValue = declaration.DefaultBool;
            if (declaration.Type == MaterialPropertyType::Texture)
            {
                const std::string defaultPath = IsBuiltinTextureToken(declaration.DefaultTextureToken)
                    ? ToLowerCopy(declaration.DefaultTextureToken)
                    : ResolveTexturePathFromToken(declaration.DefaultTextureToken);
                property.TextureValue = defaultPath.empty() ? declaration.DefaultTextureToken : defaultPath;
            }
            mShaderProperties.push_back(std::move(property));
        }
    }

    void Material::SyncExposedPropertiesFromLegacyPbr()
    {
        if (!bUseShaderProperties)
            return;

        auto findProperty = [this](std::initializer_list<const char*> names) -> MaterialRuntimeProperty*
        {
            for (const char* name : names)
            {
                const auto iterator = std::find_if(
                    mShaderProperties.begin(),
                    mShaderProperties.end(),
                    [name](const MaterialRuntimeProperty& property) { return property.Decl.Name == name; });
                if (iterator != mShaderProperties.end())
                    return &*iterator;
            }
            return nullptr;
        };

        auto applyTexture = [&](std::initializer_list<const char*> names, const std::string& path, const char* defaultToken)
        {
            if (MaterialRuntimeProperty* property = findProperty(names))
            {
                property->TextureValue = path.empty()
                    ? (IsBuiltinTextureToken(defaultToken) ? ToLowerCopy(defaultToken) : ResolveTexturePathFromToken(defaultToken))
                    : CollapseBuiltinTexturePath(path);
            }
        };
        const auto seedSampling = [&](std::initializer_list<const char*> names, const auto& info) {
            if (auto* property = findProperty(names)) { property->Sampling = info.sampling; property->Sampling.texCoord = info.texCoord; }
        };
        seedSampling({"g_AlbedoMap", "g_BaseColorMap"}, mPbrMatRes.pbrMetallicRoughness.baseColorTexture);
        seedSampling({"g_NormalMap"}, mPbrMatRes.normalTexture);
        seedSampling({"g_MetallicRoughnessMap"}, mPbrMatRes.pbrMetallicRoughness.metallicRoughnessTexture);
        seedSampling({"g_AoMap"}, mPbrMatRes.occlusionTexture);
        seedSampling({"g_EmissiveMap"}, mPbrMatRes.emissiveTexture);
        auto applyFloat = [&](std::initializer_list<const char*> names, float value)
        {
            if (MaterialRuntimeProperty* property = findProperty(names))
                property->FloatValue = value;
        };
        auto applyColor = [&](std::initializer_list<const char*> names, const glm::vec4& value)
        {
            if (MaterialRuntimeProperty* property = findProperty(names))
                property->ColorValue = value;
        };

        applyTexture({ "g_AlbedoMap", "g_BaseColorMap" }, mPbrMatRes.pbrMetallicRoughness.baseColorTexture.textureRes.textureSourcePath, "white");
        applyTexture({ "g_NormalMap" }, mPbrMatRes.normalTexture.textureRes.textureSourcePath, "normal");
        applyTexture({ "g_MetallicRoughnessMap" }, mPbrMatRes.pbrMetallicRoughness.metallicRoughnessTexture.textureRes.textureSourcePath, "white");
        applyTexture({ "g_AoMap" }, mPbrMatRes.occlusionTexture.textureRes.textureSourcePath, "white");
        applyTexture({ "g_EmissiveMap" }, mPbrMatRes.emissiveTexture.textureRes.textureSourcePath, "black");
        applyColor({ "g_BaseColorFactor", "baseColorTint" }, baseColorFactor);
        applyColor({ "g_EmissiveFactor" }, glm::vec4(emissiveFactor, 1.0f));
        applyFloat({ "g_MetallicFactor", "metallic" }, metallicFactor);
        applyFloat({ "g_RoughnessFactor", "roughness" }, roughnessFactor);
        applyFloat({ "g_NormalStrength", "normalStrength" }, mPbrMatRes.normalTexture.scale);
        applyFloat({ "g_AoStrength" }, aoStrength);
        applyFloat({ "g_AlphaClip" }, mPbrMatRes.alphaMode == "MASK" ? 1.0f : 0.0f);
        applyFloat({ "g_AlphaCutoff" }, mPbrMatRes.alphaCutoff);
    }

    void Material::SyncLegacyPbrFromExposedProperties()
    {
        if (!bUseShaderProperties)
            return;

        auto findProperty = [this](std::initializer_list<const char*> names) -> const MaterialRuntimeProperty*
        {
            for (const char* name : names)
            {
                const auto iterator = std::find_if(
                    mShaderProperties.begin(),
                    mShaderProperties.end(),
                    [name](const MaterialRuntimeProperty& property) { return property.Decl.Name == name; });
                if (iterator != mShaderProperties.end())
                    return &*iterator;
            }
            return nullptr;
        };

        auto readTexture = [&](std::initializer_list<const char*> names, MaterialTextureSemantic semantic)
        {
            const MaterialRuntimeProperty* property = findProperty(names);
            if (!property)
                return;

            std::string path = NormalizePath(property->TextureValue);
            const bool bLooksLikePath = path.find('\\') != std::string::npos ||
                path.find('/') != std::string::npos || path.find('.') != std::string::npos;
            if (!bLooksLikePath)
                path = ResolveTexturePathFromToken(path);

            const std::string defaultPath = ResolveTexturePathFromToken(property->Decl.DefaultTextureToken);
            SetTexturePath(semantic, path, !path.empty() && path != NormalizePath(defaultPath));
            const auto applySampling = [&](auto& target) { target.sampling = property->Sampling; target.texCoord = property->Sampling.texCoord; };
            switch (semantic) {
            case MaterialTextureSemantic::BaseColor: applySampling(mPbrMatRes.pbrMetallicRoughness.baseColorTexture); break;
            case MaterialTextureSemantic::Normal: applySampling(mPbrMatRes.normalTexture); break;
            case MaterialTextureSemantic::MetallicRoughness: applySampling(mPbrMatRes.pbrMetallicRoughness.metallicRoughnessTexture); break;
            case MaterialTextureSemantic::Occlusion: applySampling(mPbrMatRes.occlusionTexture); break;
            case MaterialTextureSemantic::Emissive: applySampling(mPbrMatRes.emissiveTexture); break;
            }
        };

        if (const MaterialRuntimeProperty* property = findProperty({ "g_BaseColorFactor", "baseColorTint" }))
            baseColorFactor = mPbrMatRes.pbrMetallicRoughness.baseColorFactor = property->ColorValue;
        if (const MaterialRuntimeProperty* property = findProperty({ "g_EmissiveFactor" }))
            emissiveFactor = mPbrMatRes.emissiveFactor = glm::vec3(property->ColorValue);
        if (const MaterialRuntimeProperty* property = findProperty({ "g_MetallicFactor", "metallic" }))
            metallicFactor = mPbrMatRes.pbrMetallicRoughness.metallicFactor = property->FloatValue;
        if (const MaterialRuntimeProperty* property = findProperty({ "g_RoughnessFactor", "roughness" }))
            roughnessFactor = mPbrMatRes.pbrMetallicRoughness.roughnessFactor = property->FloatValue;
        if (const MaterialRuntimeProperty* property = findProperty({ "g_NormalStrength", "normalStrength" }))
            mPbrMatRes.normalTexture.scale = property->FloatValue;
        if (const MaterialRuntimeProperty* property = findProperty({ "g_AoStrength" }))
            aoStrength = mPbrMatRes.occlusionTexture.strength = property->FloatValue;
        if (mSchemaVersion == 1)
        {
            if (const MaterialRuntimeProperty* property = findProperty({ "g_AlphaClip" }))
            {
                // Alpha clipping distinguishes MASK from OPAQUE, but disabling
                // it must not erase an authored legacy BLEND material.
                if (property->FloatValue >= 0.5f) mPbrMatRes.alphaMode = "MASK";
                else if (mPbrMatRes.alphaMode != "BLEND") mPbrMatRes.alphaMode = "OPAQUE";
            }
            if (const MaterialRuntimeProperty* property = findProperty({ "g_AlphaCutoff" }))
                mPbrMatRes.alphaCutoff = property->FloatValue;
        }

        readTexture({ "g_AlbedoMap", "g_BaseColorMap" }, MaterialTextureSemantic::BaseColor);
        readTexture({ "g_NormalMap" }, MaterialTextureSemantic::Normal);
        readTexture({ "g_MetallicRoughnessMap" }, MaterialTextureSemantic::MetallicRoughness);
        readTexture({ "g_AoMap" }, MaterialTextureSemantic::Occlusion);
        readTexture({ "g_EmissiveMap" }, MaterialTextureSemantic::Emissive);
    }

    void Material::SyncRenderStateToExposedProperties()
    {
        if (!bUseShaderProperties)
            return;
        for (MaterialRuntimeProperty& property : mShaderProperties)
        {
            if (property.Decl.Name == "g_AlphaClip" &&
                property.Decl.Type == MaterialPropertyType::Float)
            {
                property.FloatValue = mPbrMatRes.alphaMode == "MASK" ? 1.0f : 0.0f;
            }
            else if (property.Decl.Name == "g_AlphaCutoff" &&
                property.Decl.Type == MaterialPropertyType::Float)
            {
                property.FloatValue = std::clamp(mPbrMatRes.alphaCutoff, 0.0f, 1.0f);
            }
        }
    }

    void Material::LoadExposedPropertiesFromMaterialJson(const Json& propertiesJson)
    {
        if (!bUseShaderProperties ||
            !propertiesJson.contains("ExposedProperties") ||
            !propertiesJson["ExposedProperties"].is_object())
        {
            return;
        }

        const Json& exposedJson = propertiesJson["ExposedProperties"];
        std::unordered_set<std::string> consumedNames;
        for (MaterialRuntimeProperty& property : mShaderProperties)
        {
            auto valueIterator = exposedJson.find(property.Decl.Name);
            if (valueIterator == exposedJson.end())
            {
                const std::string canonicalName = CanonicalPropertyName(property.Decl.Name);
                for (auto aliasIterator = exposedJson.begin();
                    aliasIterator != exposedJson.end();
                    ++aliasIterator)
                {
                    if (CanonicalPropertyName(aliasIterator.key()) == canonicalName)
                    {
                        valueIterator = aliasIterator;
                        break;
                    }
                }
            }
            if (valueIterator == exposedJson.end())
                continue;

            consumedNames.insert(valueIterator.key());
            const Json& value = valueIterator.value();
            bool bLoaded = false;
            if (property.Decl.Type == MaterialPropertyType::Float && value.is_number())
            {
                const float parsed = value.get<float>();
                if (std::isfinite(parsed))
                {
                    property.FloatValue = parsed;
                    bLoaded = true;
                }
            }
            else if (property.Decl.Type == MaterialPropertyType::Color && value.is_array() && value.size() >= 3)
            {
                bLoaded = TryReadVector(value, property.ColorValue, 3, 4);
            }
            else if (property.Decl.Type == MaterialPropertyType::Float2)
            {
                bLoaded = TryReadVector(value, property.ColorValue, 2, 2);
            }
            else if (property.Decl.Type == MaterialPropertyType::Float3)
            {
                bLoaded = TryReadVector(value, property.ColorValue, 3, 3);
            }
            else if (property.Decl.Type == MaterialPropertyType::Bool && value.is_boolean())
            {
                property.BoolValue = value.get<bool>();
                bLoaded = true;
            }
            else if (property.Decl.Type == MaterialPropertyType::Int && value.is_number_integer())
            {
                property.IntValue = value.get<int>();
                bLoaded = true;
            }
            else if (property.Decl.Type == MaterialPropertyType::Texture && value.is_string())
            {
                const std::string serializedPath = value.get<std::string>();
                if (IsBuiltinTextureToken(serializedPath))
                {
                    property.TextureValue = ToLowerCopy(serializedPath);
                    bLoaded = true;
                }
                else
                {
                    std::string normalized;
                    if (Utils::TryNormalizeProjectRelativePath(serializedPath, normalized) &&
                        ToLowerCopy(normalized).starts_with("assets/") &&
                        !Utils::Utf8ToPath(normalized).extension().empty())
                    {
                        property.TextureValue = CollapseBuiltinTexturePath(normalized);
                        bLoaded = true;
                    }
                }
            }

            if (!bLoaded)
            {
                PreserveOrphan(
                    mOrphanedParameters,
                    valueIterator.key(),
                    Json{
                    { "Type", "legacy" },
                    { "Value", value },
                    },
                    "incompatible-value");
            }
        }

        for (auto iterator = exposedJson.begin(); iterator != exposedJson.end(); ++iterator)
        {
            if (consumedNames.contains(iterator.key()))
                continue;
            PreserveOrphan(
                mOrphanedParameters,
                iterator.key(),
                Json{
                { "Type", "legacy" },
                { "Value", iterator.value() },
                },
                "unknown-property");
        }
    }

    void Material::LoadTypedPropertiesFromMaterialJson(const Json& materialJson)
    {
        const auto findProperty = [this](
            const std::string& serializedName,
            const std::string& serializedStableId) -> MaterialRuntimeProperty*
        {
            if (!serializedStableId.empty())
            {
                const auto stableIterator = std::find_if(
                    mShaderProperties.begin(),
                    mShaderProperties.end(),
                    [&](const MaterialRuntimeProperty& property)
                    {
                        return property.Decl.StableId == serializedStableId;
                    });
                if (stableIterator != mShaderProperties.end())
                    return &*stableIterator;
            }

            const std::string canonicalName = CanonicalPropertyName(serializedName);
            const auto iterator = std::find_if(
                mShaderProperties.begin(),
                mShaderProperties.end(),
                [&](const MaterialRuntimeProperty& property)
                {
                    const bool bNameMatches =
                        property.Decl.StableId == serializedName ||
                        property.Decl.Name == serializedName ||
                        CanonicalPropertyName(property.Decl.Name) == canonicalName;
                    // Older V2 assets used the property name as their StableId.
                    // A real, different StableId must still protect a reused
                    // name from inheriting another generation's value.
                    return bNameMatches &&
                        (serializedStableId.empty() ||
                            serializedStableId == serializedName ||
                            property.Decl.StableId.empty());
                });
            return iterator == mShaderProperties.end() ? nullptr : &*iterator;
        };

        const auto preserveOrphan = [this](
            const std::string& name,
            const Json& value,
            const char* reason)
        {
            PreserveOrphan(mOrphanedParameters, name, value, reason);
        };

        std::unordered_map<std::string, std::string> claimedStableIds;
        std::unordered_map<const MaterialRuntimeProperty*, std::string> claimedProperties;
        const auto rejectDuplicate = [&claimedStableIds, &claimedProperties](
            const std::string& stableId,
            const MaterialRuntimeProperty* property,
            const std::string& location,
            std::string& outFirstLocation)
        {
            if (!stableId.empty())
            {
                const auto iterator = claimedStableIds.find(stableId);
                if (iterator != claimedStableIds.end())
                {
                    outFirstLocation = iterator->second;
                    return true;
                }
            }
            if (property)
            {
                const auto iterator = claimedProperties.find(property);
                if (iterator != claimedProperties.end())
                {
                    outFirstLocation = iterator->second;
                    return true;
                }
            }
            if (!stableId.empty())
                claimedStableIds.emplace(stableId, location);
            if (property)
                claimedProperties.emplace(property, location);
            return false;
        };

        if (materialJson.contains("Parameters") && materialJson["Parameters"].is_object())
        {
            for (auto iterator = materialJson["Parameters"].begin();
                iterator != materialJson["Parameters"].end();
                ++iterator)
            {
                const Json& entry = iterator.value();
                const std::string stableId = entry.is_object() &&
                    entry.contains("StableId") && entry["StableId"].is_string()
                    ? entry["StableId"].get<std::string>()
                    : std::string{};
                MaterialRuntimeProperty* property = findProperty(iterator.key(), stableId);
                const std::string location = "Parameters." + iterator.key();
                std::string firstLocation;
                if (rejectDuplicate(
                        stableId,
                        property && property->Decl.Type != MaterialPropertyType::Texture
                            ? property
                            : nullptr,
                        location,
                        firstLocation))
                {
                    PreserveOrphan(
                        mOrphanedParameters,
                        iterator.key(),
                        entry,
                        "duplicate-stable-id",
                        firstLocation);
                    HE_CORE_WARN(
                        "Material '{0}' ignored duplicate property '{1}' targeting '{2}'.",
                        mMatResPath,
                        location,
                        firstLocation);
                    continue;
                }
                if (!property || property->Decl.Type == MaterialPropertyType::Texture)
                {
                    preserveOrphan(iterator.key(), entry, "unknown-property");
                    continue;
                }
                if (!entry.is_object() || !entry.contains("Type") ||
                    !entry["Type"].is_string() || !entry.contains("Value") ||
                    !IsTypeCompatible(entry["Type"].get<std::string>(), property->Decl.Type))
                {
                    preserveOrphan(iterator.key(), entry, "incompatible-type");
                    continue;
                }

                const Json& value = entry["Value"];
                const bool bLoaded = TryReadRuntimePropertyValue(*property, value);
                if (!bLoaded)
                    preserveOrphan(iterator.key(), entry, "incompatible-value");
            }
        }

        if (materialJson.contains("Textures") && materialJson["Textures"].is_object())
        {
            for (auto iterator = materialJson["Textures"].begin();
                iterator != materialJson["Textures"].end();
                ++iterator)
            {
                const Json& entry = iterator.value();
                const std::string stableId = entry.is_object() &&
                    entry.contains("StableId") && entry["StableId"].is_string()
                    ? entry["StableId"].get<std::string>()
                    : std::string{};
                MaterialRuntimeProperty* property = findProperty(iterator.key(), stableId);
                const std::string location = "Textures." + iterator.key();
                std::string firstLocation;
                if (rejectDuplicate(
                        stableId,
                        property && property->Decl.Type == MaterialPropertyType::Texture
                            ? property
                            : nullptr,
                        location,
                        firstLocation))
                {
                    PreserveOrphan(
                        mOrphanedParameters,
                        iterator.key(),
                        entry,
                        "duplicate-stable-id",
                        firstLocation);
                    HE_CORE_WARN(
                        "Material '{0}' ignored duplicate property '{1}' targeting '{2}'.",
                        mMatResPath,
                        location,
                        firstLocation);
                    continue;
                }
                if (!property || property->Decl.Type != MaterialPropertyType::Texture ||
                    !entry.is_object() ||
                    (entry.contains("Type") &&
                        (!entry["Type"].is_string() ||
                            !IsTypeCompatible(entry["Type"].get<std::string>(), MaterialPropertyType::Texture))))
                {
                    preserveOrphan(iterator.key(), entry, "incompatible-type");
                    continue;
                }

                property->Sampling = {};
                if (entry.contains("Sampling")) Serializer::Deserialize(entry.at("Sampling"), property->Sampling);
                (void)PackTextureSampling(property->Sampling);
                std::string normalizedFallback;
                if (!TryReadTextureEntry(
                        entry,
                        property->Decl.DefaultTextureToken,
                        property->TextureValue,
                        &normalizedFallback))
                {
                    preserveOrphan(iterator.key(), entry, "unsafe-texture-path");
                    continue;
                }
                if (property->Decl.DefaultTextureToken.empty())
                    property->Decl.DefaultTextureToken = normalizedFallback;
                if (property->Decl.TextureSemantic.empty() &&
                    entry.contains("Semantic") && entry["Semantic"].is_string())
                {
                    property->Decl.TextureSemantic = entry["Semantic"].get<std::string>();
                }
                if (property->Decl.ColorSpace.empty() &&
                    entry.contains("ColorSpace") && entry["ColorSpace"].is_string())
                {
                    property->Decl.ColorSpace = entry["ColorSpace"].get<std::string>();
                }
            }
        }
    }

    void Material::MigrateRuntimeProperties(
        const std::vector<MaterialRuntimeProperty>& previousProperties,
        const Json& previousOrphans)
    {
        mOrphanedParameters = previousOrphans.is_object()
            ? previousOrphans
            : Json::object();
        std::unordered_set<size_t> consumed;
        for (MaterialRuntimeProperty& property : mShaderProperties)
        {
            const auto findPrevious = [&](bool bStableIdPass) -> std::optional<size_t>
            {
                for (size_t index = 0; index < previousProperties.size(); ++index)
                {
                    if (consumed.contains(index))
                        continue;
                    const MaterialRuntimeProperty& previous = previousProperties[index];
                    const bool bIdentityMatches = bStableIdPass
                        ? StableIdentityMatches(property.Decl, previous.Decl)
                        : CompatibleNameIdentityMatches(property.Decl, previous.Decl);
                    if (bIdentityMatches && property.Decl.Type == previous.Decl.Type)
                        return index;
                }
                return std::nullopt;
            };

            std::optional<size_t> previousIndex = findPrevious(true);
            if (!previousIndex)
                previousIndex = findPrevious(false);
            if (previousIndex)
            {
                const MaterialRuntimeProperty& previous =
                    previousProperties[*previousIndex];
                bool bValueAccepted = true;
                if (property.Decl.Type == MaterialPropertyType::Texture)
                {
                    property.Sampling = previous.Sampling;
                    bValueAccepted = TryNormalizeTextureReference(
                        previous.TextureValue,
                        true,
                        property.TextureValue);
                }
                else
                {
                    property.FloatValue = previous.FloatValue;
                    property.ColorValue = previous.ColorValue;
                    property.IntValue = previous.IntValue;
                    property.BoolValue = previous.BoolValue;
                }
                if (bValueAccepted)
                {
                    consumed.insert(*previousIndex);
                    continue;
                }
            }

            TryRestoreRuntimePropertyFromOrphans(property, mOrphanedParameters);
        }

        for (size_t index = 0; index < previousProperties.size(); ++index)
        {
            if (consumed.contains(index))
                continue;
            const MaterialRuntimeProperty& property = previousProperties[index];
            const bool bIdentityStillExistsWithDifferentType =
                std::any_of(
                    mShaderProperties.begin(),
                    mShaderProperties.end(),
                    [&](const MaterialRuntimeProperty& current)
                    {
                        const bool bSameIdentity =
                            StableIdentityMatches(current.Decl, property.Decl) ||
                            CompatibleNameIdentityMatches(current.Decl, property.Decl);
                        return bSameIdentity && property.Decl.Type != current.Decl.Type;
                    });
            const char* reason = bIdentityStillExistsWithDifferentType
                ? "incompatible-type"
                : "shader-layout-changed";
            Json entry{
                { "StableId", property.Decl.StableId },
                { "Type", PropertyTypeName(property.Decl.Type) },
            };
            switch (property.Decl.Type)
            {
            case MaterialPropertyType::Float:
                entry["Value"] = property.FloatValue;
                break;
            case MaterialPropertyType::Float2:
                entry["Value"] = { property.ColorValue.x, property.ColorValue.y };
                break;
            case MaterialPropertyType::Float3:
                entry["Value"] = {
                    property.ColorValue.x, property.ColorValue.y, property.ColorValue.z };
                break;
            case MaterialPropertyType::Color:
                entry["Value"] = {
                    property.ColorValue.x,
                    property.ColorValue.y,
                    property.ColorValue.z,
                    property.ColorValue.w };
                break;
            case MaterialPropertyType::Bool:
                entry["Value"] = property.BoolValue;
                break;
            case MaterialPropertyType::Int:
                entry["Value"] = property.IntValue;
                break;
            case MaterialPropertyType::Texture:
                Serializer::Serialize(entry["Sampling"], property.Sampling);
                entry["Path"] = IsBuiltinTextureToken(property.TextureValue)
                    ? std::string{}
                    : property.TextureValue;
                entry["Fallback"] = IsBuiltinTextureToken(property.TextureValue)
                    ? property.TextureValue
                    : property.Decl.DefaultTextureToken;
                break;
            }
            PreserveOrphan(
                mOrphanedParameters,
                property.Decl.Name,
                std::move(entry),
                reason);
        }
    }

    Json Material::SerializeTypedParameters() const
    {
        Json parameters = Json::object();
        for (const MaterialRuntimeProperty& property : mShaderProperties)
        {
            if (property.Decl.Type == MaterialPropertyType::Texture)
                continue;
            Json entry{
                { "StableId", property.Decl.StableId },
                { "Type", PropertyTypeName(property.Decl.Type) },
            };
            switch (property.Decl.Type)
            {
            case MaterialPropertyType::Float:
                entry["Value"] = property.FloatValue;
                break;
            case MaterialPropertyType::Float2:
                entry["Value"] = { property.ColorValue.x, property.ColorValue.y };
                break;
            case MaterialPropertyType::Float3:
                entry["Value"] = {
                    property.ColorValue.x, property.ColorValue.y, property.ColorValue.z };
                break;
            case MaterialPropertyType::Color:
                entry["Value"] = {
                    property.ColorValue.x,
                    property.ColorValue.y,
                    property.ColorValue.z,
                    property.ColorValue.w };
                break;
            case MaterialPropertyType::Bool:
                entry["Value"] = property.BoolValue;
                break;
            case MaterialPropertyType::Int:
                entry["Value"] = property.IntValue;
                break;
            case MaterialPropertyType::Texture:
                break;
            }
            parameters[property.Decl.Name] = std::move(entry);
        }
        return parameters;
    }

    Json Material::SerializeTypedTextures() const
    {
        Json textures = Json::object();
        for (const MaterialRuntimeProperty& property : mShaderProperties)
        {
            if (property.Decl.Type != MaterialPropertyType::Texture)
                continue;
            const bool bBuiltin = IsBuiltinTextureToken(property.TextureValue);
            Json sampling; Serializer::Serialize(sampling, property.Sampling);
            textures[property.Decl.Name] = {
                { "StableId", property.Decl.StableId },
                { "Type", "texture2d" },
                { "Path", bBuiltin ? std::string{} : property.TextureValue },
                { "Fallback", bBuiltin
                    ? property.TextureValue
                    : property.Decl.DefaultTextureToken },
                { "ColorSpace", property.Decl.ColorSpace },
                { "Semantic", property.Decl.TextureSemantic },
                { "Sampling", sampling },
            };
        }
        return textures;
    }
}
