#pragma once

#include <filesystem>

namespace HEngine
{
	class GltfExporter
	{
	public:
		void Export(const std::filesystem::path& assetPath, const std::filesystem::path& outDir);
	};
}