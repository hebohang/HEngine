#include "MeshImporter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace HEngine
{
	MeshImporter::MeshImporter()
	{
		mAssimpImporter = CreateScope<AssimpImporter>();
	}

	bool MeshImporter::Import(const std::filesystem::path& assetPath, const std::filesystem::path& outDir)
	{
		if (!mAssimpImporter)
		{
			HE_CORE_ERROR("MeshImporter: module is not initialized");
			return false;
		}

		std::string ext = assetPath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		static constexpr std::array<const char*, 6> kAssimpFormats = {
			".fbx", ".gltf", ".glb", ".obj", ".dae", ".blend"
		};

		const bool bSupported = std::find(kAssimpFormats.begin(), kAssimpFormats.end(), ext) != kAssimpFormats.end();
		if (!bSupported)
		{
			HE_CORE_ERROR("MeshImporter: unsupported format '{0}'", assetPath.extension().string());
			return false;
		}

		return mAssimpImporter->Import(assetPath, outDir);
	}
}
