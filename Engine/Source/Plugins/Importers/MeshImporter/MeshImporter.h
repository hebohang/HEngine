#pragma once

#include "AssimpImporter.h"
#include "Runtime/Core/Plugins/Services/MeshImporterService.h"
#include "Runtime/Core/Log/Log.h"

#include <filesystem>

namespace HEngine
{
	class MeshImporter final : public IMeshImporterService
	{
	public:
		MeshImporter();

		[[nodiscard]] bool Import(
			const std::filesystem::path& assetPath,
			const std::filesystem::path& outDir) override;
	private:
		Scope<AssimpImporter> mAssimpImporter;
	};
}
