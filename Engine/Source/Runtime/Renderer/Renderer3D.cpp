#include "hepch.h"

#include "Runtime/Renderer/Renderer3D.h"
#include "Runtime/Renderer/Texture.h"
#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/RenderCommand.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"

#include <glm/glm.hpp>

namespace HEngine
{
	struct CubeVertex
	{
		glm::vec3 Pos;
		//glm::vec4 Color;
		glm::vec2 Coord;
		glm::vec3 Normal;
	};

	struct Renderer3DData
	{
		static const int MaxTriangles = 30000;
		static const int MaxVertices = MaxTriangles * 3;
		static const int MaxIndices = MaxVertices * 1000;

		Ref<VertexArray> mVertexArray;
		Ref<VertexBuffer> mVertexBuffer;
		Ref<IndexBuffer> mIndexBuffer;
		Ref<Shader> mShader;
	};

	static Renderer3DData sData;

	void Renderer3D::Init()
	{
		sData.mVertexArray = VertexArray::Create();

		sData.mVertexBuffer = VertexBuffer::Create(Renderer3DData::MaxVertices * sizeof(CubeVertex));

		sData.mVertexBuffer->SetLayout({
			{ShaderDataType::Float3, "a_Pos"},
			//{ShaderDataType::Float3, "a_Color"},
			{ShaderDataType::Float2, "a_Coord"},
			{ShaderDataType::Float3, "a_Normal"},
			}
		);
		sData.mVertexArray->AddVertexBuffer(sData.mVertexBuffer);

		int IndicesOffset = 0;
		uint32_t* CubeIndices = new uint32_t[12];
		for (int i = 0; i < 2; ++i)
		{
			CubeIndices[i * 6] = IndicesOffset;
			CubeIndices[i * 6 + 1] = IndicesOffset + 1;
			CubeIndices[i * 6 + 2] = IndicesOffset + 2;
			CubeIndices[i * 6 + 3] = IndicesOffset + 3;
			CubeIndices[i * 6 + 4] = IndicesOffset + 4;
			CubeIndices[i * 6 + 5] = IndicesOffset + 5;
			IndicesOffset += 6;
		}

		sData.mIndexBuffer = IndexBuffer::Create(CubeIndices, 12);
		sData.mVertexArray->SetIndexBuffer(sData.mIndexBuffer);
		delete[] CubeIndices;

		//Shader
		sData.mShader = Shader::Create(AssetManager::GetInstance().GetFullPath("Shaders/Cube.glsl"));
		sData.mShader->Bind();
	}

	void Renderer3D::Shutdown()
	{
	}

	void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
	{
		sData.mShader->Bind();
		sData.mVertexArray->Bind();

		glm::mat4 viewProj = camera.GetProjection() * glm::inverse(transform);

		sData.mShader->SetMat4("u_ViewProjection", viewProj);
	}

	void Renderer3D::BeginScene(const EditorCamera& camera)
	{
		sData.mShader->Bind();
		sData.mVertexArray->Bind();

		glm::mat4 viewProj = camera.GetViewProjection();

		sData.mShader->SetMat4("u_ViewProjection", viewProj);
	}

	void Renderer3D::EndScene()
	{
		RenderCommand::DrawIndexed(sData.mVertexArray, 12);
	}

	void Renderer3D::DrawCube(const glm::mat4& transform)
	{
		float CubeVertices[] = {
			//Pos				 Coord		 Normal
			-0.5f, -0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
			 0.5f, -0.5f,  0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
			 0.5f,  0.5f,  0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
			 0.5f,  0.5f,  0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
			-0.5f,  0.5f,  0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
			-0.5f, -0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,

			 0.5f, -0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
			 0.5f, -0.5f, -0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
			 0.5f,  0.5f, -0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
			 0.5f,  0.5f, -0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
			 0.5f,  0.5f,  0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
			 0.5f, -0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f
		};
		sData.mVertexBuffer->SetData(CubeVertices, (uint32_t)sizeof(CubeVertices));
		sData.mShader->SetMat4("u_Model", transform);
	}
}