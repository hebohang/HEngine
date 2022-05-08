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

        void Add(const std::string& name, const Ref<Texture2D>& mem)
        {
            HE_CORE_ASSERT(iconLibrary.find(name) == iconLibrary.end() && "Already have this member in Library!");
            iconLibrary[name] = mem;
        }
        [[nodiscard]] Ref<Texture2D> Get(const std::string& name)
        {
            HE_CORE_ASSERT(iconLibrary.find(name) != iconLibrary.end() && "Can't find this member in Library!");
            return iconLibrary[name];
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