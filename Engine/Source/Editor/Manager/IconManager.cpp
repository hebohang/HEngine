#include "Editor/Manager/IconManager.h"

#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace HEngine
{
	namespace
	{
		constexpr const char* kThumbnailCachePrefix = "##thumb_";
		constexpr uint32_t kMaximumThumbnailDimension = 256;

		[[nodiscard]] uint32_t NormalizeThumbnailDimension(uint32_t requestedSize) noexcept
		{
			const uint32_t clampedSize = std::clamp(
				requestedSize,
				1u,
				kMaximumThumbnailDimension);
			if (clampedSize <= 64)
				return 64;
			if (clampedSize <= 128)
				return 128;
			return kMaximumThumbnailDimension;
		}

		[[nodiscard]] std::string MakeThumbnailCacheKey(
			const std::string& path,
			uint32_t thumbSize,
			bool bSrgb)
		{
			return std::string(kThumbnailCachePrefix) +
				(bSrgb ? "srgb_" : "display_") +
				std::to_string(NormalizeThumbnailDimension(thumbSize)) + "_" + path;
		}
	}

    ImGuiTextureHandle IconManager::LoadOrFindTexture(const std::string& path)
    {
        if (const auto existing = mIconLibrary.find(path); existing != mIconLibrary.end())
            return existing->second;

        const ImGuiTextureHandle handle =
            Application::GetInstance().LoadImGuiTexture(path, true);
        mIconLibrary.emplace(path, handle);
        return handle;
    }

    ImGuiTextureHandle IconManager::LoadOrFindThumbnail(
        const std::string& path,
        uint32_t thumbSize,
        bool bSrgb)
    {
		thumbSize = NormalizeThumbnailDimension(thumbSize);
		const std::string cacheKey = MakeThumbnailCacheKey(path, thumbSize, bSrgb);
		if (const auto existing = mIconLibrary.find(cacheKey); existing != mIconLibrary.end())
		{
			TouchThumbnail(cacheKey);
			return existing->second;
		}

		// Keep previews bounded to a small GPU texture. The full-resolution asset
		// is still used by runtime materials; only the editor preview is resized.
        ImGuiTextureHandle handle;
        try
        {
			handle = Application::GetInstance().LoadImGuiTexture(path, bSrgb, thumbSize);
        }
        catch (const std::exception& exception)
        {
            HE_CORE_WARN("Editor thumbnail load failed for '{}': {}", path, exception.what());
            return GetFileIcon();
		}
		mIconLibrary.emplace(cacheKey, handle);
		TouchThumbnail(cacheKey);
		TrimThumbnailCache();
		return handle;
	}

	ImGuiTextureHandle IconManager::TryGetThumbnail(
		const std::string& path,
		uint32_t thumbSize,
		bool bSrgb)
	{
		thumbSize = NormalizeThumbnailDimension(thumbSize);
		const std::string cacheKey = MakeThumbnailCacheKey(path, thumbSize, bSrgb);
		const auto existing = mIconLibrary.find(cacheKey);
		if (existing == mIconLibrary.end())
			return {};
		TouchThumbnail(cacheKey);
		return existing->second;
	}

	void IconManager::ClearThumbnailCache()
	{
		for (const std::string& cacheKey : mThumbnailLru)
		{
			const auto found = mIconLibrary.find(cacheKey);
			if (found == mIconLibrary.end())
				continue;
			Application::GetInstance().ReleaseImGuiTexture(found->second);
			mIconLibrary.erase(found);
		}
		mThumbnailLru.clear();
		mThumbnailLruLookup.clear();
	}

    void IconManager::Shutdown() noexcept
    {
        for (const auto& [name, handle] : mIconLibrary)
        {
            try
            {
                Application::GetInstance().ReleaseImGuiTexture(handle);
            }
            catch (const std::exception& exception)
            {
                Log::WriteFallbackError("Editor icon release failed.", exception.what());
            }
            catch (...)
            {
                Log::WriteFallbackError("Editor icon release failed.", "unknown exception");
            }
		}
		mIconLibrary.clear();
		mThumbnailLru.clear();
		mThumbnailLruLookup.clear();
    }

    ImGuiTextureHandle IconManager::Get(const std::string& name) const
    {
        const auto found = mIconLibrary.find(name);
        if (found == mIconLibrary.end())
            throw std::out_of_range("Unknown editor icon: " + name);
        return found->second;
    }

    ImGuiTextureHandle IconManager::TryGet(const std::string& name) const noexcept
    {
        const auto found = mIconLibrary.find(name);
        return found == mIconLibrary.end() ? ImGuiTextureHandle{} : found->second;
    }

	void IconManager::Add(const std::string& name, const std::string& path)
    {
        if (mIconLibrary.contains(name))
            throw std::logic_error("Duplicate editor icon registration: " + name);
		mIconLibrary.emplace(
            name,
            Application::GetInstance().LoadImGuiTexture(path, true));
	}

	void IconManager::TouchThumbnail(const std::string& cacheKey)
	{
		if (const auto existing = mThumbnailLruLookup.find(cacheKey);
			existing != mThumbnailLruLookup.end())
		{
			mThumbnailLru.splice(mThumbnailLru.begin(), mThumbnailLru, existing->second);
			existing->second = mThumbnailLru.begin();
			return;
		}

		mThumbnailLru.push_front(cacheKey);
		mThumbnailLruLookup.emplace(cacheKey, mThumbnailLru.begin());
	}

	void IconManager::TrimThumbnailCache()
	{
		while (mThumbnailLru.size() > ThumbnailCapacity)
		{
			const std::string cacheKey = mThumbnailLru.back();
			mThumbnailLru.pop_back();
			mThumbnailLruLookup.erase(cacheKey);
			const auto found = mIconLibrary.find(cacheKey);
			if (found == mIconLibrary.end())
				continue;
			Application::GetInstance().ReleaseImGuiTexture(found->second);
			mIconLibrary.erase(found);
		}
	}

    IconManager::IconManager()
    {
        Add("EntityIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Entity.png").string());
        Add("NullTextrue", ConfigManager::GetFullPath("Assets/Textures/DefaultTexture.png").string());
        Add("SettingIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Setting.png").string());
        Add("DirectoryIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/DirectoryIcon.png").string());
        Add("FileIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/FileIcon.png").string());
        Add("PlayIcon", ConfigManager::GetEngineFullPath("Resources/Icons/PlayButton.png").string());
        Add("StopIcon", ConfigManager::GetEngineFullPath("Resources/Icons/StopButton.png").string());
        Add("TransIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Toolbar/Trans.png").string());
        Add("RotateIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Toolbar/Rotate.png").string());
        Add("ScaleIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Toolbar/Scale.png").string());
        Add("MusicIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Music.png").string());
        Add("MaterialIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Material.png").string());
        Add("MeshIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Mesh.png").string());

        Add("EmptyEntityIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Entity/EmptyEntity.png").string());
        Add("PointLightEntityIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Entity/PointLight.png").string());
        Add("DirLightEntityIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Entity/DirectionalLight.png").string());
        Add("CameraEntityIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Entity/Camera.png").string());
        Add("AudioEntityIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Entity/Audio.png").string());
        Add("MeshEntityIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Entity/Mesh.png").string());

        Add("TransformCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/Transform.png").string());
        Add("CameraCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/Camera.png").string());
        Add("MeshCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/MeshRenderer.png").string());
        Add("PointLightCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/PointLight.png").string());
        Add("DirLightCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/DirectionalLight.png").string());
        Add("RigidbodyCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/Rigidbody.png").string());
        Add("ColliderCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/Collider.png").string());
        Add("AnimatorCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/Animator.png").string());
        Add("CharacterCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/Character.png").string());
        Add("SpriteCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/SpriteRenderer.png").string());
        Add("CircleCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/CircleRenderer.png").string());
        Add("ScriptCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/Script.png").string());
        Add("SoundCompIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Component/Sound.png").string());

        Add("VisibleIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Hierarchy/Visible.png").string());
        Add("HiddenIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Hierarchy/Hidden.png").string());
        Add("LockedIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Hierarchy/Locked.png").string());
        Add("UnlockedIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Hierarchy/Unlocked.png").string());

        Add("SaveIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Toolbar/Save.png").string());
        Add("UndoIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Toolbar/Undo.png").string());
        Add("RedoIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Toolbar/Redo.png").string());
        Add("GridSnapIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Toolbar/GridSnap.png").string());
        Add("LocalSpaceIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Toolbar/LocalSpace.png").string());
        Add("PerspectiveIcon", ConfigManager::GetEngineFullPath("Resources/Icons/Toolbar/Perspective.png").string());

        Add("AnimIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/AnimIcon.png").string());
        Add("AnimCtrlIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/AnimCtrlIcon.png").string());
        Add("SkeletonIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/SkeletonIcon.png").string());
        Add("SceneIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/SceneIcon.png").string());
        Add("PrefabIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/PrefabIcon.png").string());
        Add("EffectIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/EffectIcon.png").string());
        Add("UiIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/UiIcon.png").string());
        Add("BehaviorTreeIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/BehaviorTreeIcon.png").string());
        Add("ScriptIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/ScriptIcon.png").string());
        Add("ShaderIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/ShaderIcon.png").string());
        Add("FontIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/FontIcon.png").string());
        Add("UiMarkupIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/UiMarkupIcon.png").string());
        Add("MeshSourceIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/MeshSourceIcon.png").string());
        Add("ConfigIcon", ConfigManager::GetEngineFullPath("Resources/Icons/ContentBrowser/ConfigIcon.png").string());
    }
}
