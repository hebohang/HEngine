#pragma once

#include "Runtime/Renderer/Texture.h"
#include "Runtime/Renderer/Shader.h"
#include "Runtime/Library/ShaderLibrary.h"
#include "Runtime/Library/TextureLibrary.h"

#include <unordered_map>

namespace HEngine
{
    // from https://stackoverflow.com/questions/18837857/cant-use-enum-class-as-unordered-map-key
    struct EnumClassHash
    {
        template <typename T>
        std::size_t operator()(T t) const
        {
            return static_cast<std::size_t>(t);
        }
    };

    enum class TextureType
    {
        Albedo = 0,
        Normal,
        Metalness,
        Roughness,
        AmbientOcclusion,
        Specular,
        Height,
        Emission,
    };

    struct MaterialTexture
    {
        Ref<Texture2D> texture2d = nullptr;
        TextureType type;
        std::string path;
    };

    class Material
    {
    public:
        Material() { Initialize(); };
        Material(Ref<Shader> shader) : mShader(shader) { Initialize(); };
    public:
        void SetShader(Ref<Shader> shader) { mShader = shader; }
        [[nodiscard]] Ref<Shader> GetShader() { return mShader; }

        void AddTexture(TextureType type, Ref<Texture2D> texture) 
        {
            HE_CORE_ASSERT(mTexMap.find(type) == mTexMap.end());
            mTexMap[type] = texture;
        }

        [[nodiscard]] Ref<Texture2D> GetTexture(TextureType type) { return mTexMap[type]; }
    private:
        void Initialize();
    public:
        std::vector<MaterialTexture> mTextures;

        bool bUseAlbedoMap = false;
        glm::vec4 col = { 1.0f, 1.0f, 1.0f, 1.0f }; // 0 ~ 1
        Ref<Texture2D> albedoRGBA = Texture2D::Create(1, 1);
        Ref<Texture2D> mAlbedoMap = Library<Texture2D>::GetInstance().GetDefaultTexture();

        bool bUseNormalMap = false;
        Ref<Texture2D> mNormalMap = Library<Texture2D>::GetInstance().Get("DefaultNormal");

        bool bUseMetallicMap = false;
        float metallic = 0.1f;
        Ref<Texture2D> metallicRGBA = Texture2D::Create(1, 1);
        Ref<Texture2D> mMetallicMap = Library<Texture2D>::GetInstance().Get("DefaultMetallicRoughness");

        bool bUseRoughnessMap = false;
        float roughness = 0.9f;
        Ref<Texture2D> roughnessRGBA = Texture2D::Create(1, 1);
        Ref<Texture2D> mRoughnessMap = Library<Texture2D>::GetInstance().Get("DefaultMetallicRoughness");

        bool bUseAoMap = false;
        Ref<Texture2D> mAoMap = Library<Texture2D>::GetInstance().GetWhiteTexture();
    private:
        Ref<Shader> mShader;
        std::unordered_map<TextureType, Ref<Texture2D>, EnumClassHash> mTexMap;
    };

}