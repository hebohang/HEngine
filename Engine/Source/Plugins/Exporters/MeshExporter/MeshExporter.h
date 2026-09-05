#pragma once

#include "GltfExporter.h"
#include "Runtime/Core/Modules/ModuleInterface.h"
#include "Runtime/Core/Base/Base.h"

#include <filesystem>

namespace HEngine
{
	class MeshExporter : public IModule
	{
	public:
		void StartModule() override;

		void Export(const std::filesystem::path& assetPath, const std::filesystem::path& outDir);
	private:
		Scope<GltfExporter> mGltfExporter = nullptr;
	};
}