#pragma once 

#include "Runtime/Core/Base/PublicSingleton.h"
#include "Runtime/Renderer/Texture.h"

#include <unordered_map>

namespace HEngine
{
    class IconManager : public PublicSingleton<IconManager>
    {
    public:
        IconManager();

        Ref<Texture2D> LoadOrFindTexture(const std::string& path)
        {
            if (iconLibrary.find(path) != iconLibrary.end())
            {
                return iconLibrary[path];
            }
            iconLibrary[path] = Texture2D::Create(path);
            return iconLibrary[path];
        }

        [[nodiscard]] Ref<Texture2D> GetNullTexture() { return mNullTexture; }
        [[nodiscard]] Ref<Texture2D> GetDirectoryIcon() { return mDirectoryIcon; }
        [[nodiscard]] Ref<Texture2D> GetFileIcon() { return mFileIcon; }
        [[nodiscard]] Ref<Texture2D> GetSettingIcon() { return mSettingIcon; }
    private:
        std::unordered_map<std::string, Ref<Texture2D>> iconLibrary;

        Ref<Texture2D> mNullTexture;
        Ref<Texture2D> mDirectoryIcon;
        Ref<Texture2D> mFileIcon;
        Ref<Texture2D> mSettingIcon;
    };
}