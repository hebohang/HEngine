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
		std::filesystem::path mCurrentDirectory;

		Ref<Texture2D> mDirectoryIcon;
		Ref<Texture2D> mFileIcon;
	};
}
