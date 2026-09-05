#pragma once

#include "SubMesh.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#if !defined(HE_PACKAGED_GAME)
class aiNode;
class aiMesh;
class aiScene;
class aiMaterial;
#endif

namespace HEngine 
{
	class Mesh;

	enum class MeshDecodeError : std::uint8_t
	{
		None = 0,
		Cancelled,
		MetadataFileError,
		MetadataTooLarge,
		InvalidResource,
		AllocationFailed
	};

	struct DecodedSubMesh
	{
		std::vector<StaticVertex> StaticVertices;
		std::vector<SkinnedVertex> SkinnedVertices;
		std::vector<std::uint32_t> Indices;
		std::uint32_t MaterialIndex = 0;

		[[nodiscard]] bool IsSkinned() const noexcept { return !SkinnedVertices.empty(); }
	};

	struct DecodedModel
	{
		std::string SkeletonPath;
		std::vector<std::string> MaterialPaths;
		std::vector<DecodedSubMesh> SubMeshes;
	};

	struct MeshDecodeResult
	{
		MeshDecodeError Code = MeshDecodeError::None;
		std::string Message;
		DecodedModel Model;

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return Code == MeshDecodeError::None;
		}
	};

	class MeshLoader
	{
	public:
		static void LoadModel(Mesh* mesh, const std::string& path);
		// Worker-safe CPU-only decode. The project root is captured explicitly so
		// background work never reads mutable ConfigManager state.
		[[nodiscard]] static MeshDecodeResult DecodeModelHMESH(
			const std::filesystem::path& fullPath,
			const std::filesystem::path& projectRoot,
			std::stop_token stopToken = {});
        // Main-thread only: publishes decoded CPU assets to the live Mesh.
        static void FinalizeModelHMESH(Mesh* mesh, DecodedModel&& decodedModel);

	private:
		// gltf
		static void LoadModelHMESH(Mesh* mesh, const std::filesystem::path& path);

	private:
		// assimp
#if !defined(HE_PACKAGED_GAME)
		static void LoadModelASSIMP(Mesh* mesh, const std::string& path);

		static void ProcessNodeASSIMP(Mesh* mesh, aiNode* node, const aiScene* scene, uint32_t& subMeshIndex);

		template <typename Vertex>
		static Ref<SubMesh> ProcessMeshASSIMP(Mesh* mesh, aiMesh* assimpMesh, const aiScene* scene, uint32_t& subMeshIndex);

		static void loadMaterialTexturesASSIMP(Mesh* mesh, aiMaterial* mat, unsigned int type, uint32_t& subMeshIndex);
#endif
	};
}
