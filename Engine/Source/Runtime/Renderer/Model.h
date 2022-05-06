#pragma once

#include "Runtime/Core/Base/Base.h"

#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/StaticMesh.h"
#include "Runtime/Renderer/Texture.h"
#include "Runtime/Renderer/Material.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace HEngine 
{
	class Model
	{
	public:
		Model() = default;
		Model(const std::string& path)
			: mMaterial(CreateRef<Material>(ShaderLibrary::GetDefaultShader()))
		{
			LoadModel(path);
		}		
		
		Model(const std::string& path, Ref<Shader> shader)
			: mMaterial(CreateRef<Material>(shader))
		{
			LoadModel(path);
		}

		void SetShader(Ref<Shader> shader) { mMaterial->SetShader(shader); };
		void Draw(const glm::mat4& transform, int entityID);
		void Draw(const glm::mat4& transform, Ref<Shader> shader, int entityID);

		void Draw();
	private:
		void LoadModel(const std::string& path);
		void ProcessNode(aiNode* node, const aiScene* scene);
		StaticMesh ProcessMesh(aiMesh* mesh, const aiScene* scene);
	private:
		Ref<Material> mMaterial = CreateRef<Material>();
		std::vector<StaticMesh> mMeshes;
		std::string mDirectory;
		std::vector<Texture2D> loadMaterialTextures(aiMaterial* mat, aiTextureType type, std::string typeName);
	};
}