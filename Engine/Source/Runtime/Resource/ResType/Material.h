#pragma once
#include "Runtime/Core/Meta/Reflection/Reflection.h"
#include "Texture.h"
#include "TextureSampling.h"
#include <glm/glm.hpp>

#include <vector>

namespace HEngine
{
    // refer to https://github.com/syoyo/tinygltf/blob/release/tiny_gltf.h and https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.pdf#page=125&zoom=100,0,397

    // TODO: extensions and extras
    REFLECTION_TYPE(TextureInfo)
    CLASS(TextureInfo, Fields)
    {
        REFLECTION_BODY(TextureInfo);
    public:
        TextureInfo() = default;
        //TextureInfo(const std::string& path) : textureResPath(path) {}

    public:
        //std::string textureResPath; // required
        TextureRes textureRes;
        TextureSampling sampling;
        int texCoord = 0;           // The set index of texture's TEXCOORD attribute used for
                                    // texture coordinate mapping.
    };

    REFLECTION_TYPE(NormalTextureInfo)
    CLASS(NormalTextureInfo, Fields)
    {
        REFLECTION_BODY(NormalTextureInfo);
    public:
        NormalTextureInfo() = default;
        //NormalTextureInfo(const std::string & path) : textureResPath(path) {}

    public:
        //std::string textureResPath; // required
        TextureRes textureRes;
        TextureSampling sampling;
        int texCoord = 0;           // The set index of texture's TEXCOORD attribute used for
                                    // texture coordinate mapping.
        float scale = 1.0f;         // scaledNormal = normalize((<sampled normal texture value>
                                    // * 2.0 - 1.0) * vec3(<normal scale>, <normal scale>, 1.0))
    };

    REFLECTION_TYPE(OcclusionTextureInfo)
    CLASS(OcclusionTextureInfo, Fields)
    {
        REFLECTION_BODY(OcclusionTextureInfo);
    public:
        OcclusionTextureInfo() = default;
        //OcclusionTextureInfo(const std::string & path) : textureResPath(path) {}

    public:
        //std::string textureResPath;  // required
        TextureRes textureRes;
        TextureSampling sampling;
        int texCoord = 0;            // The set index of texture's TEXCOORD attribute used for
                                     // texture coordinate mapping.
        float strength = 1.0f;       // occludedColor = lerp(color, color * <sampled occlusion
                                     // texture value>, <occlusion strength>)
    };

    // class defined in glTF 2.0 spec.
    REFLECTION_TYPE(PbrMetallicRoughness)
    CLASS(PbrMetallicRoughness, Fields)
    {
        REFLECTION_BODY(PbrMetallicRoughness);

    public:
        // TODO: PROPERTY Range
        PROPERTY(Range = [0.0f, 1.0f])
        glm::vec4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;

        TextureInfo baseColorTexture;
        TextureInfo metallicRoughnessTexture;
    };

    // related to gltf 2.0
    // BasePBR material
    REFLECTION_TYPE(PbrMaterialRes)
    CLASS(PbrMaterialRes, Fields)
    {
        REFLECTION_BODY(PbrMaterialRes);
    public:
        std::string name;

        PbrMetallicRoughness pbrMetallicRoughness;
        NormalTextureInfo normalTexture;
        OcclusionTextureInfo occlusionTexture;
        TextureInfo emissiveTexture;

        glm::vec3 emissiveFactor = { 0.0f, 0.0f, 0.0f };
        std::string alphaMode = "OPAQUE";  // OPAQUE MASK BLEND
        float alphaCutoff = 0.5f;
        bool doubleSided = false;

        bool bUseAlbedoMap = false;
        bool bUseNormalMap = false;
        bool bUseMetallicRoughnessMap = false;
        bool bUseAoMap = false;
        bool bUseEmissiveMap = false;
    };

    REFLECTION_TYPE(MaterialRes)
    CLASS(MaterialRes, Fields)
    {
        REFLECTION_BODY(MaterialRes);
    public:
        MaterialRes()
            : Parameters(Json::object())
            , Textures(Json::object())
            , OrphanedParameters(Json::object())
            , Properties(Json::object())
        {
        }

        static std::string GetExtName() { return "hmat"; }
    public:
        int SchemaVersion = 2;
        std::string Type = "Default";
        std::string PassName = "BasePBR";
        std::string ShaderPath = "Shaders\\BasePBR.hlsl";
        std::vector<std::string> VariantKeywords;
        std::string Domain = "Surface";
        std::string ShadingModel = "Lit";
        std::string BlendMode = "Opaque";
        std::string CullMode = "Back";
        bool DepthWrite = true;
        float AlphaCutoff = 0.5f;
        Json Parameters;
        Json Textures;
        Json OrphanedParameters;
        Json Properties;
    };
}
