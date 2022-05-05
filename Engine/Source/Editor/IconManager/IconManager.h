#pragma once 

#include "Runtime/Core/Base/PublicSingleton.h"
#include "Runtime/Renderer/Texture.h"

namespace HEngine
{
    class IconManager : public PublicSingleton<IconManager>
    {
    public:
        IconManager();

        [[nodiscard]] Ref<Texture2D> GetNullTexture() { return mNullTexture; }
        [[nodiscard]] Ref<Texture2D> GetDirectoryIcon() { return mDirectoryIcon; }
        [[nodiscard]] Ref<Texture2D> GetFileIcon() { return mFileIcon; }
        [[nodiscard]] Ref<Texture2D> GetSettingIcon() { return mSettingIcon; }
    private:
        Ref<Texture2D> mNullTexture;
        Ref<Texture2D> mDirectoryIcon;
        Ref<Texture2D> mFileIcon;
        Ref<Texture2D> mSettingIcon;
    };
}