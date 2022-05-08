#include "hepch.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Library/TextureLibrary.h"

namespace HEngine
{
    Library<Texture2D>::Library()
    {
        Add("DefaultTexture",           Texture2D::Create(std::string("Assets/Textures/DefaultTexture.png")));
        Add("DefaultNormal",            Texture2D::Create(std::string("Assets/Textures/DefaultNormal.png")));
        Add("DefaultMetallicRoughness", Texture2D::Create(std::string("Assets/Textures/DefaultMetallicRoughness.png")));
        Add("DefaultHdr", Texture2D::Create(std::string("Assets/Textures/DefaultTexture.png")));

        Ref<Texture2D> whiteTexture = Texture2D::Create(1, 1);
        uint32_t whiteTextureData = 0xffffffff;
        whiteTexture->SetData(&whiteTextureData, sizeof(uint32_t));
        Add("WhiteTexture", whiteTexture);
    }

    Ref<Texture2D> Library<Texture2D>::GetDefaultTexture()
    {
        return mLibrary["DefaultTexture"];
    }
}