#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Editor/IconManager/IconManager.h"

namespace HEngine
{
    IconManager::IconManager()
    {
        Add("EntityIcon", Texture2D::Create(AssetManager::GetFullPath("Resources/Icons/Entity.png")));
        Add("NullTextrue", Texture2D::Create(AssetManager::GetFullPath("Assets/Textures/DefaultTexture.png")));
        Add("SettingIcon", Texture2D::Create(std::string(("Resources/Icons/Setting.png"))));
        Add("DirectoryIcon", Texture2D::Create(std::string(("Resources/Icons/ContentBrowser/DirectoryIcon.png"))));
        Add("FileIcon", Texture2D::Create(std::string(("Resources/Icons/ContentBrowser/FileIcon.png"))));
        Add("PlayIcon", Texture2D::Create(std::string(("Resources/Icons/PlayButton.png"))));
        Add("StopIcon", Texture2D::Create(std::string(("Resources/Icons/StopButton.png"))));
    }
}