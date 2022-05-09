#pragma once

#include "Runtime/Core/Base/Base.h"

#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/StaticMesh.h"
#include "Runtime/Renderer/Texture.h"
#include "Runtime/Renderer/Material.h"
#include "Runtime/Library/ShaderLibrary.h"
#include "Runtime/Library/TextureLibrary.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <optional>

namespace HEngine 
{
	class Model
	{
	public:
		Model() = default;
		Model(const std::string& path)
			: mMaterial(CreateRef<Material>(Library<Shader>::GetInstance().GetDefaultShader()))
		{
			LoadModel(path);
		}		
		
		Model(const std::string& path, Ref<Shader> shader)
			: mMaterial(CreateRef<Material>(shader))
		{
			LoadModel(path);
		}

		void SetShader(Ref<Shader> shader) { mMaterial->SetShader(shader); };
		void Draw(const glm::mat4& transform, const glm::vec3& cameraPos, const glm::mat4& viewMatrix, int entityID);
		void Draw(const glm::mat4& transform, const glm::vec3& cameraPos, const glm::mat4& viewMatrix, Ref<Shader> shader, int entityID);

		void Draw();
	private:
		void LoadModel(const std::string& path);
		void ProcessNode(aiNode* node, const aiScene* scene);
		StaticMesh ProcessMesh(aiMesh* mesh, const aiScene* scene);
		std::optional<std::vector<MaterialTexture>> loadMaterialTextures(aiMaterial* mat, aiTextureType type);
	public:
		Ref<Texture2D> mAlbedoMap = Library<Texture2D>::GetInstance().GetDefaultTexture();
		Ref<Texture2D> mNormalMap = Library<Texture2D>::GetInstance().Get("DefaultNormal");
		Ref<Texture2D> mMetallicMap = Library<Texture2D>::GetInstance().Get("DefaultMetallicRoughness");
		Ref<Texture2D> mRoughnessMap = Library<Texture2D>::GetInstance().Get("DefaultMetallicRoughness");
		Ref<Texture2D> mAoMap = Library<Texture2D>::GetInstance().Get("WhiteTexture");
	private:
		Ref<Material> mMaterial = CreateRef<Material>();
		std::vector<StaticMesh> mMeshes;
		std::string mDirectory;
	};
}