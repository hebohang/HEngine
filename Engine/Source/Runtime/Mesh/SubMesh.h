#pragma once

#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/VertexBuffer.h"
#include "Runtime/Renderer/IndexBuffer.h"
#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/Material.h"

#define MAX_BONE_INFLUENCE 4

namespace HEngine 
{
	class Mesh; 

	struct StaticVertex
	{
		glm::vec3 Pos;
		glm::vec3 Normal;
		glm::vec2 TexCoord;
		glm::vec3 Tangent;
		glm::vec3 Bitangent;

		int EntityID;
	};

	struct SkinnedVertex 
	{
		glm::vec3 Pos;
		glm::vec3 Normal;
		glm::vec2 TexCoord;
		glm::vec3 Tangent;
		glm::vec3 Bitangent;

		int EntityID;

		int mBoneIDs[MAX_BONE_INFLUENCE];
		float mWeights[MAX_BONE_INFLUENCE];
	};

	class SubMesh
	{
	public:
		SubMesh(const std::vector<StaticVertex>& vertices, const std::vector<uint32_t> indices);
		SubMesh(const std::vector<SkinnedVertex>& vertices, const std::vector<uint32_t> indices);
		SubMesh(const std::vector<StaticVertex>& vertices, const std::vector<uint32_t> indices, const std::vector<MaterialTexture>& textures);
		SubMesh(const std::vector<SkinnedVertex>& vertices, const std::vector<uint32_t> indices, const std::vector<MaterialTexture>& textures);

		void Draw(const glm::mat4& transform, const glm::vec3& cameraPos, const Ref<Shader>& shader, int entityID, Mesh* model);

		void Draw();
	private:
		void SetupMesh(int entityID);
	private:
		std::vector<MaterialTexture> mTextures;

		std::vector<StaticVertex> mStaticVertices;
		std::vector<SkinnedVertex> mSkinnedVertices;

		std::vector<uint32_t> mIndices;

		Ref<VertexArray> mVertexArray;
		Ref<VertexBuffer> mVB;
		Ref<IndexBuffer> mIB;

		int mEntityID = -1;
	};
}