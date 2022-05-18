#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/Mesh/Mesh.h"

#include <filesystem>

namespace HEngine
{
    class MeshComponent : public ComponentBase
    {
    public:
		MeshComponent() { mMesh = CreateRef<Mesh>(); };
		MeshComponent(const MeshComponent&) = default;
		MeshComponent(const std::string & path)
			: Path(path), mMesh(CreateRef<Mesh>(path))
		{
		}
		//MeshComponent(const std::filesystem::path& path)
		//	: Path(path)
		//{
		//}

		std::string Path;
		Ref<Mesh> mMesh;
    };
}