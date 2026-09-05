#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/Graphics/Gui/NvrhiImGuiRenderer.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

namespace HEngine
{
    // Editor image registry backed by Application's typed NVRHI texture owner.
    // It stores opaque, generation-checked ImGui handles rather than native SRVs.
    class IconManager : public PublicSingleton<IconManager>
    {
    public:
        IconManager();

        [[nodiscard]] ImGuiTextureHandle LoadOrFindTexture(const std::string& path);
        [[nodiscard]] ImGuiTextureHandle LoadOrFindThumbnail(
            const std::string& path,
            uint32_t thumbSize = 128,
            bool bSrgb = true);
		[[nodiscard]] ImGuiTextureHandle TryGetThumbnail(
			const std::string& path,
			uint32_t thumbSize = 128,
			bool bSrgb = true);

        void ClearThumbnailCache();
        void Shutdown() noexcept;

        [[nodiscard]] ImGuiTextureHandle Get(const std::string& name) const;
        [[nodiscard]] ImGuiTextureHandle TryGet(const std::string& name) const noexcept;

        [[nodiscard]] ImGuiTextureHandle GetNullTexture() const { return Get("NullTextrue"); }
        [[nodiscard]] ImGuiTextureHandle GetDirectoryIcon() const { return Get("DirectoryIcon"); }
        [[nodiscard]] ImGuiTextureHandle GetFileIcon() const { return Get("FileIcon"); }
        [[nodiscard]] ImGuiTextureHandle GetSettingIcon() const { return Get("SettingIcon"); }
        [[nodiscard]] ImGuiTextureHandle GetPlayIcon() const { return Get("PlayIcon"); }
        [[nodiscard]] ImGuiTextureHandle GetStopIcon() const { return Get("StopIcon"); }

        [[nodiscard]] static ImTextureID ToImTextureID(ImGuiTextureHandle handle) noexcept
        {
            return NvrhiImGuiRenderer::ToImTextureID(handle);
        }

    private:
        void Add(const std::string& name, const std::string& path);
		void TouchThumbnail(const std::string& cacheKey);
		void TrimThumbnailCache();

        std::unordered_map<std::string, ImGuiTextureHandle> mIconLibrary;
		std::list<std::string> mThumbnailLru;
		std::unordered_map<std::string, std::list<std::string>::iterator> mThumbnailLruLookup;
		static constexpr std::size_t ThumbnailCapacity = 256;
    };
}
