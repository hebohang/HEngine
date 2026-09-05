#include "MeshExporter.h"

namespace HEngine
{
	void MeshExporter::StartModule()
	{
		HE_CORE_INFO("Mesh Exporter Module Start!");
		//mGltfImporter = CreateScope<GltfExporter>();
	}

	void MeshExporter::Export(const std::filesystem::path& assetPath, const std::filesystem::path& outDir)
	{
		if (assetPath.extension().string() == ".gltf")
		{
			//mGltfImporter->Export(assetPath, outDir);
		}
		else if (assetPath.extension().string() == ".fbx")
		{
			HE_CORE_INFO("Export Fbx!");
		}
		else
		{
			HE_CORE_ERROR("Engine do not support this format!");
		}
	}
}