#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/Mesh/Mesh.h"

#include <filesystem>

namespace HEngine
{
    class MeshComponent : public ComponentBase
    {
    public:
		MeshComponent() = default;
		MeshComponent(const MeshComponent&) = default;
		MeshComponent(const std::string & path)
			: Path(path), Mesh(path)
		{
		}
		//MeshComponent(const std::filesystem::path& path)
		//	: Path(path)
		//{
		//}

		std::string Path;
		Mesh Mesh;
    };
}