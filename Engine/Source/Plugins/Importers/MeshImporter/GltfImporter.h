#pragma once

#include <filesystem>

namespace HEngine
{
	class GltfImporter
	{
	public:
		void Import(const std::filesystem::path& assetPath, const std::filesystem::path& outDir);
	};
}