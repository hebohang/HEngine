#include "Hepch.h"

#include "BasePbrMaterialBuilder.h"

#include "_generated/serializer/Material.gen.h"
#include "_generated/serializer/TextureSampling.gen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
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

        std::string ToUpperCopy(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::toupper(character));
            });
            return value;
        }

        bool IsBuiltinTextureToken(std::string_view value)
        {
            const std::string lower = ToLowerCopy(std::string(value));
            return lower == "white" || lower == "black" || lower == "normal";
        }

        Json MakeParameter(std::string_view name, std::string_view type, Json value)
        {
            return {
                { "StableId", name },
                { "Type", type },
                { "Value", std::move(value) },
            };
        }

        void AddTexture(
            MaterialRes& material,
            Json& exposedProperties,
            std::string_view name,
            const std::string& sourcePath,
            bool bEnabled,
            std::string_view fallback,
            std::string_view colorSpace,
            std::string_view semantic)
        {
            std::string exposedValue(fallback);
            if (bEnabled && !sourcePath.empty())
            {
                exposedValue = IsBuiltinTextureToken(sourcePath)
                    ? ToLowerCopy(sourcePath)
                    : sourcePath;
            }

            const bool bBuiltin = IsBuiltinTextureToken(exposedValue);
            material.Textures[std::string(name)] = {
                { "StableId", name },
                { "Type", "texture2d" },
                { "Path", bBuiltin ? std::string{} : exposedValue },
                { "Fallback", bBuiltin ? exposedValue : std::string(fallback) },
                { "ColorSpace", colorSpace },
                { "Semantic", semantic },
            };
            exposedProperties[std::string(name)] = std::move(exposedValue);
        }
    }

    MaterialRes BuildBasePbrMaterialResV2(const PbrMaterialRes& pbrMaterial)
    {
        PbrMaterialRes legacyPbr = pbrMaterial;
        const std::string alphaMode = ToUpperCopy(legacyPbr.alphaMode);
        legacyPbr.alphaMode = alphaMode == "MASK" || alphaMode == "BLEND"
            ? alphaMode
            : "OPAQUE";
        legacyPbr.alphaCutoff = std::isfinite(legacyPbr.alphaCutoff)
            ? std::clamp(legacyPbr.alphaCutoff, 0.0f, 1.0f)
            : 0.5f;

        MaterialRes material;
        material.SchemaVersion = 2;
        material.Type = "Default";
        material.PassName = "BasePBR";
        material.ShaderPath = "Shaders/BasePBR.hlsl";
        material.VariantKeywords.clear();
        material.Domain = "Surface";
        material.ShadingModel = "Lit";
        material.BlendMode = legacyPbr.alphaMode == "BLEND"
            ? "Transparent"
            : legacyPbr.alphaMode == "MASK" ? "Mask" : "Opaque";
        material.CullMode = legacyPbr.doubleSided ? "None" : "Back";
        material.DepthWrite = legacyPbr.alphaMode != "BLEND";
        material.AlphaCutoff = legacyPbr.alphaCutoff;
        material.Parameters = Json::object();
        material.Textures = Json::object();
        material.OrphanedParameters = Json::object();
        material.Properties = Json::object();

        const glm::vec4& baseColor = legacyPbr.pbrMetallicRoughness.baseColorFactor;
        const glm::vec3& emissive = legacyPbr.emissiveFactor;
        material.Parameters["g_BaseColorFactor"] = MakeParameter(
            "g_BaseColorFactor", "color",
            Json::array({ baseColor.r, baseColor.g, baseColor.b, baseColor.a }));
        material.Parameters["g_MetallicFactor"] = MakeParameter(
            "g_MetallicFactor", "float", legacyPbr.pbrMetallicRoughness.metallicFactor);
        material.Parameters["g_RoughnessFactor"] = MakeParameter(
            "g_RoughnessFactor", "float", legacyPbr.pbrMetallicRoughness.roughnessFactor);
        material.Parameters["g_AoStrength"] = MakeParameter(
            "g_AoStrength", "float", legacyPbr.occlusionTexture.strength);
        material.Parameters["g_NormalStrength"] = MakeParameter(
            "g_NormalStrength", "float", legacyPbr.normalTexture.scale);
        material.Parameters["g_EmissiveFactor"] = MakeParameter(
            "g_EmissiveFactor", "color",
            Json::array({ emissive.r, emissive.g, emissive.b, 1.0f }));
        material.Parameters["g_AlphaClip"] = MakeParameter(
            "g_AlphaClip", "float", legacyPbr.alphaMode == "MASK" ? 1.0f : 0.0f);
        material.Parameters["g_AlphaCutoff"] = MakeParameter(
            "g_AlphaCutoff", "float", legacyPbr.alphaCutoff);

        Json exposedProperties = Json::object();
        AddTexture(
            material,
            exposedProperties,
            "g_AlbedoMap",
            legacyPbr.pbrMetallicRoughness.baseColorTexture.textureRes.textureSourcePath,
            legacyPbr.bUseAlbedoMap,
            "white",
            "SRGB",
            "BaseColor");
        exposedProperties["g_BaseColorFactor"] = {
            baseColor.r, baseColor.g, baseColor.b, baseColor.a };
        AddTexture(
            material,
            exposedProperties,
            "g_MetallicRoughnessMap",
            legacyPbr.pbrMetallicRoughness.metallicRoughnessTexture.textureRes.textureSourcePath,
            legacyPbr.bUseMetallicRoughnessMap,
            "white",
            "Linear",
            "MetallicRoughness");
        AddTexture(
            material,
            exposedProperties,
            "g_MetallicMap",
            {},
            false,
            "white",
            "Linear",
            "Data");
        AddTexture(
            material,
            exposedProperties,
            "g_RoughnessMap",
            {},
            false,
            "white",
            "Linear",
            "Data");
        exposedProperties["g_MetallicFactor"] = legacyPbr.pbrMetallicRoughness.metallicFactor;
        exposedProperties["g_RoughnessFactor"] = legacyPbr.pbrMetallicRoughness.roughnessFactor;
        AddTexture(
            material,
            exposedProperties,
            "g_AoMap",
            legacyPbr.occlusionTexture.textureRes.textureSourcePath,
            legacyPbr.bUseAoMap,
            "white",
            "Linear",
            "Occlusion");
        exposedProperties["g_AoStrength"] = legacyPbr.occlusionTexture.strength;
        AddTexture(
            material,
            exposedProperties,
            "g_NormalMap",
            legacyPbr.normalTexture.textureRes.textureSourcePath,
            legacyPbr.bUseNormalMap,
            "normal",
            "Linear",
            "Normal");
        exposedProperties["g_NormalStrength"] = legacyPbr.normalTexture.scale;
        AddTexture(
            material,
            exposedProperties,
            "g_EmissiveMap",
            legacyPbr.emissiveTexture.textureRes.textureSourcePath,
            legacyPbr.bUseEmissiveMap,
            "black",
            "SRGB",
            "Emissive");
        exposedProperties["g_EmissiveFactor"] = {
            emissive.r, emissive.g, emissive.b, 1.0f };
        exposedProperties["g_AlphaClip"] = legacyPbr.alphaMode == "MASK" ? 1.0f : 0.0f;
        exposedProperties["g_AlphaCutoff"] = legacyPbr.alphaCutoff;

        const auto sampling = [&](const char* name, const auto& info) {
            TextureSampling value = info.sampling;
            value.texCoord = info.texCoord;
            (void)PackTextureSampling(value);
            Serializer::Serialize(material.Textures[name]["Sampling"], value);
        };
        sampling("g_AlbedoMap", legacyPbr.pbrMetallicRoughness.baseColorTexture);
        sampling("g_NormalMap", legacyPbr.normalTexture);
        sampling("g_MetallicRoughnessMap", legacyPbr.pbrMetallicRoughness.metallicRoughnessTexture);
        sampling("g_AoMap", legacyPbr.occlusionTexture);
        sampling("g_EmissiveMap", legacyPbr.emissiveTexture);
        Serializer::Serialize(material.Properties, legacyPbr);
        material.Properties["ExposedProperties"] = std::move(exposedProperties);
        return material;
    }
}
