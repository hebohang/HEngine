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
		std::filesystem::path m_CurrentDirectory;
	};
}
