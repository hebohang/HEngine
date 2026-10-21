#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Resource/ResType/Mesh.h"

#include <filesystem>
#include <string>
#include <vector>

namespace HEngine
{
	REFLECTION_TYPE(MeshComponent)
	CLASS(MeshComponent : public ComponentBase, Fields)
    {
		REFLECTION_COMPONENT_BODY(MeshComponent);
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

		std::string Path = "None";

		// TODO: Use meshResPath (xxx.hmesh) not path (xxx.fbx or other raw 3d type)
		std::string meshResPath;

		// Optional per-instance material paths indexed by Mesh::mMaterial slot.
		// Empty entries retain the material authored by the shared .hmesh.
		std::vector<std::string> MaterialOverrides;

        // Optional per-instance static lighting. UV1 addresses a texture atlas;
        // coefficient rows decode the imported directional irradiance basis.
        // An empty path keeps the ordinary dynamic/IBL lighting path.
        std::string StaticLightmapPath;
        glm::vec2 StaticLightmapUvScale{ 1.0f };
        glm::vec2 StaticLightmapUvBias{ 0.0f };
        glm::vec4 StaticLightmapScale0{ 1.0f };
        glm::vec4 StaticLightmapAdd0{ 0.0f };
        glm::vec4 StaticLightmapScale1{ 1.0f };
        glm::vec4 StaticLightmapAdd1{ 0.0f };
        float StaticLightmapBlend = 1.0f;
        // 0: imported UE directional atlas; 1: native linear E/pi RGB lightmap.
        int StaticLightmapEncoding = 0;

		void ApplyMaterialOverrides();

		// Runtime-only presentation override. This is intentionally excluded from
		// reflection and scene serialization; menu previews restore it from UI state.
		PROPERTY(Disable)
		bool bRuntimeBlackSilhouette = false;

		PROPERTY(Disable)
		Ref<Mesh> mMesh;

		PROPERTY(Disable)
		std::vector<Ref<Material>> mAuthoredMaterials;
    };
	REGISTER_COMPONENT_TYPE(MeshComponent)
}
