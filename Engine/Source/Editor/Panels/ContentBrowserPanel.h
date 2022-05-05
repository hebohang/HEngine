#pragma once

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
		void DrawTreeRecursive(const std::filesystem::path& currentPath);
		void DrawContent();
	private:
		std::filesystem::path mCurrentDirectory;
		std::optional<std::filesystem::path> mSelectedDirectory;
	};
}
