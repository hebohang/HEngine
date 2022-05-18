#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Animation/animdata.h"
#include "Runtime/Animation/animator.h"

#include "Runtime/Renderer/Shader.h"
#include "Runtime/Mesh/SubMesh.h"
#include "Runtime/Renderer/Texture.h"
#include "Runtime/Renderer/Material.h"
#include "Runtime/Library/ShaderLibrary.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <optional>
#include <map>

namespace HEngine 
{
	class Mesh
	{
	public:
		Mesh() = default;
		Mesh(const Mesh&) = default;
		Mesh(const std::string& path)
			: mMaterial(CreateRef<Material>(Library<Shader>::GetInstance().GetDefaultShader()))
		{
			LoadModel(path);
		}		
		
		Mesh(const std::string& path, Ref<Shader> shader)
			: mMaterial(CreateRef<Material>(shader))
		{
			LoadModel(path);
		}

		void SetShader(Ref<Shader> shader) { mMaterial->SetShader(shader); };
		void Draw(const glm::mat4& transform, const glm::vec3& cameraPos, int entityID);
		void Draw(const glm::mat4& transform, const glm::vec3& cameraPos, Ref<Shader> shader, int entityID);

		void Draw();

		auto& GetBoneInfoMap() { return mBoneInfoMap; }
		int& GetBoneCount() { return mBoneCounter; }
	private:
		void LoadModel(const std::string& path);
		void ProcessNode(aiNode* node, const aiScene* scene);

		template <typename Vertex>
		SubMesh ProcessMesh(aiMesh* mesh, const aiScene* scene);
		std::optional<std::vector<MaterialTexture>> loadMaterialTextures(aiMaterial* mat, aiTextureType type);
	public:
		// Animation
		bool bAnimated = false;
		bool bPlayAnim = false;
		bool bStopAnim = false;

		Animation mAnimation;
		Animator mAnimator;

		float mAnimPlaySpeed = 1.0f;

		//std::vector<Ref<Material>> mMaterial;
		Ref<Material> mMaterial = CreateRef<Material>();
	private:
		std::vector<SubMesh> mSubMeshes;
		std::string mDirectory;

		// Animation
		int mBoneCounter = 0;
		std::map<std::string, BoneInfo> mBoneInfoMap;
	};
}