#pragma once 

#include "Runtime/Core/Base/PublicSingleton.h"
#include "Runtime/Renderer/Texture.h"

#include <unordered_map>

namespace HEngine
{
    class TextureLibrary : public PublicSingleton<TextureLibrary>
    {
    public:
        TextureLibrary();

        Ref<Texture2D> LoadOrFindTexture(const std::string& name, const std::string& path)
        {
            if (mTextureLibrary.find(path) != mTextureLibrary.end())
            {
                return mTextureLibrary[path];
            }
            mTextureLibrary[name] = Texture2D::Create(path);
            return mTextureLibrary[name];
        }

        void AddTexture(const std::string& name, const Ref<Texture2D>& texture);

        [[nodiscard]] Ref<Texture2D> GetTexture(const std::string& name);

        [[nodiscard]] Ref<Texture2D> GetDefaultTexture() { return mTextureLibrary["DefaultTexture"]; }
    private:
        std::unordered_map<std::string, Ref<Texture2D>> mTextureLibrary;
    };
}