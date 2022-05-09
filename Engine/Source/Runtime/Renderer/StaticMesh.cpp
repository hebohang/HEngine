#include "hepch.h"

#include "Runtime/Renderer/StaticMesh.h"
#include "Runtime/Renderer/RenderCommand.h"
#include "Runtime/Library/TextureLibrary.h"
#include "Runtime/Renderer/Model.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

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

	void StaticMesh::Draw(const glm::mat4& transform, const glm::vec3& cameraPos, const glm::mat4& viewMatrix, const Ref<Shader>& shader, int entityID, Model* model)
	{
		SetupMesh(entityID);

		if (ModeManager::bHdrUse)
		{
			shader->Bind();
			shader->SetMat4("model", transform);
			shader->SetFloat3("camPos", cameraPos);

			Library<CubeMapTexture>::GetInstance().Get("EnvironmentIrradiance")->Bind(0);
			Library<CubeMapTexture>::GetInstance().Get("EnvironmentPrefilter")->Bind(1);
			Library<Texture2D>::GetInstance().Get("BRDF_LUT")->Bind(2);

			model->mAlbedoMap->Bind(3);
			model->mNormalMap->Bind(4);
			model->mMetallicMap->Bind(5);
			model->mRoughnessMap->Bind(6);
			model->mAoMap->Bind(7);

			shader->SetInt("irradianceMap", 0);
			shader->SetInt("prefilterMap", 1);
			shader->SetInt("brdfLUT", 2);
			shader->SetInt("albedoMap", 3);
			shader->SetInt("normalMap", 4);
			shader->SetInt("metallicMap", 5);
			shader->SetInt("roughnessMap", 6);
			shader->SetInt("aoMap", 7);
		}
		else
		{
			shader->Bind();
			shader->SetMat4("u_Model.Transform", (transform));
			mVertexArray->Bind();

			model->mAlbedoMap->Bind(0);
			model->mNormalMap->Bind(1);
			model->mMetallicMap->Bind(2);
			model->mRoughnessMap->Bind(3);
			model->mAoMap->Bind(4);

			shader->SetInt("albedoMap", 0);
			shader->SetInt("normalMap", 1);
			shader->SetInt("metallicMap", 2);
			shader->SetInt("roughnessMap", 3);
			shader->SetInt("aoMap", 4);
			shader->SetFloat3("u_Uniform.camPos", cameraPos);
		}

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