#pragma once

#include "Runtime/Renderer/Texture.h"

#include <filesystem>

namespace HEngine
{
	class ContentBrowserPanel
	{
	public:
		ContentBrowserPanel();

		void OnImGuiRender(bool* pOpen);
	private:
		void DrawTree();
		void DrawTreeRecursive(std::filesystem::path currentPath);
		void DrawContent();
	private:
		std::filesystem::path mCurrentDirectory;
		std::optional<std::filesystem::path> mSelectedDirectory;

		Ref<Texture2D> mDirectoryIcon;
		Ref<Texture2D> mFileIcon;
	};
}
