#include "hepch.h"

#include "Runtime/Renderer/StaticMesh.h"
#include "Runtime/Renderer/RenderCommand.h"

namespace HEngine 
{
	StaticMesh::StaticMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t> indices)
		: mVertices(vertices), mIndices(indices)
	{
		mVertexArray = VertexArray::Create();

		mVB = VertexBuffer::Create(sizeof(Vertex) * vertices.size());
		mVB->SetLayout({
					{ ShaderDataType::Float3, "a_Pos"},
					{ ShaderDataType::Float3, "a_Normal"},
					{ ShaderDataType::Float3, "a_Tangent"},
					{ ShaderDataType::Float2, "a_TexCoord"},
					{ ShaderDataType::Int,	  "a_EntityID"},
			});

		mVertexArray->AddVertexBuffer(mVB);

		mIB = IndexBuffer::Create(indices.size());

		mVertexArray->SetIndexBuffer(mIB);
	}

	StaticMesh::StaticMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t> indices, const std::vector<MaterialTexture>& textures)
		: mVertices(vertices), mIndices(indices), mTextures(textures)
	{
		mVertexArray = VertexArray::Create();

		mVB = VertexBuffer::Create(sizeof(Vertex) * vertices.size());
		mVB->SetLayout({
					{ ShaderDataType::Float3, "a_Pos"},
					{ ShaderDataType::Float3, "a_Normal"},
					{ ShaderDataType::Float3, "a_Tangent"},
					{ ShaderDataType::Float2, "a_TexCoord"},
					{ ShaderDataType::Int,	  "a_EntityID"},
			});

		mVertexArray->AddVertexBuffer(mVB);

		mIB = IndexBuffer::Create(indices.size());

		mVertexArray->SetIndexBuffer(mIB);
	}

	void StaticMesh::Draw(const glm::mat4& transform, const Ref<Shader>& shader, int entityID)
	{
		SetupMesh(entityID);
		shader->Bind();
		shader->SetMat4("u_Model.Transform", (transform));
		mVertexArray->Bind();

		// Temp
		if (!mTextures.empty())
			mTextures[0].texture2d->Bind();
		shader->SetInt("texture_diffuse", 0);

		RenderCommand::DrawIndexed(mVertexArray, mIB->GetCount());
	}

	void StaticMesh::Draw()
	{
		//SetupMesh(EntityID);
		static bool bInit = true;
		if (bInit)
		{
			bInit = false;

			mVertexArray->Bind();

			mVB->SetData(mVertices.data(), sizeof(Vertex) * mVertices.size());
			mIB->SetData(mIndices.data(), mIndices.size());

			mVertexArray->Unbind();
		}
		mVertexArray->Bind();
		RenderCommand::DrawIndexed(mVertexArray, mIB->GetCount());
		mVertexArray->Unbind();
	}

	void StaticMesh::SetupMesh(int entityID)
	{
		if (mEntityID == -1)
		{
			mEntityID = entityID;
			mVertexArray->Bind();

			for (int i = 0; i < mVertices.size(); ++i)
			{
				mVertices[i].EntityID = entityID;
			}

			mVB->SetData(mVertices.data(), sizeof(Vertex) * mVertices.size());

			mIB->SetData(mIndices.data(), mIndices.size());

			mVertexArray->Unbind();
		}
	}
}