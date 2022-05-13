#pragma once

#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/VertexBuffer.h"
#include "Runtime/Renderer/IndexBuffer.h"
#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/Material.h"

#define MAX_BONE_INFLUENCE 4

namespace HEngine 
{
	class Model; 

	struct Vertex
	{
		glm::vec3 Pos;
		glm::vec3 Normal;
		glm::vec2 TexCoord;
		glm::vec3 Tangent;
		glm::vec3 Bitangent;

		int EntityID;
	};

	struct AnimatedVertex 
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 TexCoords;
		glm::vec3 Tangent;
		glm::vec3 Bitangent;
		int m_BoneIDs[MAX_BONE_INFLUENCE];
		float m_Weights[MAX_BONE_INFLUENCE];

		int EntityID;
	};

	class Mesh
	{
	public:
		Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t> indices);
		Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t> indices, const std::vector<MaterialTexture>& textures);

		void Draw(const glm::mat4& transform, const glm::vec3& cameraPos, const Ref<Shader>& shader, int entityID, Model* model);

		void Draw();
	private:
		void SetupMesh(int entityID);
	private:
		std::vector<MaterialTexture> mTextures;

		std::vector<Vertex> mStaticVertices;
		std::vector<AnimatedVertex> mAnimatedVertices;

		std::vector<uint32_t> mIndices;

		Ref<VertexArray> mVertexArray;
		Ref<VertexBuffer> mVB;
		Ref<IndexBuffer> mIB;

		int mEntityID = -1;
	};
}