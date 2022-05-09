#pragma once

#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/VertexBuffer.h"
#include "Runtime/Renderer/IndexBuffer.h"
#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/Material.h"

namespace HEngine 
{
	class Model; 

	struct Vertex
	{
		glm::vec3 Pos;
		glm::vec3 Normal;
		glm::vec3 Tangent;
		glm::vec2 TexCoord;

		//Editor
		int EntityID;
	};

	class StaticMesh
	{
	public:
		StaticMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t> indices);
		StaticMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t> indices, const std::vector<MaterialTexture>& textures);

		void Draw(const glm::mat4& transform, const glm::vec3& cameraPos, const Ref<Shader>& shader, int entityID, Model* model);

		void Draw();
	private:
		void SetupMesh(int entityID);
	private:
		std::vector<MaterialTexture> mTextures;
		std::vector<Vertex> mVertices;
		std::vector<uint32_t> mIndices;

		Ref<VertexArray> mVertexArray;
		Ref<VertexBuffer> mVB;
		Ref<IndexBuffer> mIB;

		int mEntityID = -1;
	};
}