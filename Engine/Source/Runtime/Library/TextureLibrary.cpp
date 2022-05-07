#include "hepch.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Library/TextureLibrary.h"

namespace HEngine
{
    TextureLibrary::TextureLibrary()
    {
        LoadOrFindTexture("DefaultTexture", "Assets/Textures/DefaultTexture.png");
        LoadOrFindTexture("DefaultNormal", "Assets/Textures/DefaultNormal.png");
        LoadOrFindTexture("DefaultMetallicRoughness", "Assets/Textures/DefaultMetallicRoughness.png");

        Ref<Texture2D> whiteTexture = Texture2D::Create(1, 1);
        uint32_t whiteTextureData = 0xffffffff;
        whiteTexture->SetData(&whiteTextureData, sizeof(uint32_t));
        AddTexture("WhiteTexture", whiteTexture);
    }

    void TextureLibrary::AddTexture(const std::string& name, const Ref<Texture2D>& texture)
    {
        HE_CORE_ASSERT(mTextureLibrary.find(name) == mTextureLibrary.end());
        mTextureLibrary[name] = texture;
    }

    Ref<Texture2D> TextureLibrary::GetTexture(const std::string& name)
    {
        HE_CORE_ASSERT(mTextureLibrary.find(name) != mTextureLibrary.end());
        return mTextureLibrary[name];
    }
}