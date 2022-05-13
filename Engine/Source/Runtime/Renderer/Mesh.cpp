#include "hepch.h"

#include "Runtime/Renderer/Mesh.h"
#include "Runtime/Renderer/RenderCommand.h"
#include "Runtime/Library/TextureLibrary.h"
#include "Runtime/Renderer/Model.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

namespace HEngine
{
	Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t> indices)
		: mStaticVertices(vertices), mIndices(indices)
	{
		mVertexArray = VertexArray::Create();

		mVB = VertexBuffer::Create(sizeof(Vertex) * vertices.size());
		mVB->SetLayout({
					{ ShaderDataType::Float3, "a_Pos"},
					{ ShaderDataType::Float3, "a_Normal"},
					{ ShaderDataType::Float2, "a_TexCoord"},
					{ ShaderDataType::Float3, "a_Tangent"},
					{ ShaderDataType::Float3, "a_Bitangent"},
					{ ShaderDataType::Int,	  "a_EntityID"},
			});

		mVertexArray->AddVertexBuffer(mVB);

		mIB = IndexBuffer::Create(indices.size());

		mVertexArray->SetIndexBuffer(mIB);
	}

	Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t> indices, const std::vector<MaterialTexture>& textures)
		: mStaticVertices(vertices), mIndices(indices), mTextures(textures)
	{
		mVertexArray = VertexArray::Create();

		mVB = VertexBuffer::Create(sizeof(Vertex) * vertices.size());
		mVB->SetLayout({
					{ ShaderDataType::Float3, "a_Pos"},
					{ ShaderDataType::Float3, "a_Normal"},
					{ ShaderDataType::Float2, "a_TexCoord"},
					{ ShaderDataType::Float3, "a_Tangent"},
					{ ShaderDataType::Float3, "a_Bitangent"},
					{ ShaderDataType::Int,	  "a_EntityID"},
			});

		mVertexArray->AddVertexBuffer(mVB);

		mIB = IndexBuffer::Create(indices.size());

		mVertexArray->SetIndexBuffer(mIB);
	}

	void Mesh::Draw(const glm::mat4& transform, const glm::vec3& cameraPos, const Ref<Shader>& shader, int entityID, Model* model)
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

			if (model->bUseAlbedoMap)
				model->mAlbedoMap->Bind(3);
			else
				model->albedoRGBA->Bind(3);

			if (model->bUseNormalMap)
				model->mNormalMap->Bind(4);
			else
				Library<Texture2D>::GetInstance().GetWhiteTexture()->Bind(4);

			if (model->bUseMetallicMap)
				model->mMetallicMap->Bind(5);
			else
				model->metallicRGBA->Bind(5);

			if (model->bUseRoughnessMap)
				model->mRoughnessMap->Bind(6);
			else
				model->roughnessRGBA->Bind(6);

			if (model->bUseAoMap)
				model->mAoMap->Bind(7);
			else
				Library<Texture2D>::GetInstance().GetWhiteTexture()->Bind(7);

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

			if (model->bUseAlbedoMap)
				model->mAlbedoMap->Bind(0);
			else
				model->albedoRGBA->Bind(0);

			if (model->bUseNormalMap)
				model->mNormalMap->Bind(1);
			else
				Library<Texture2D>::GetInstance().GetWhiteTexture()->Bind(1);

			if (model->bUseMetallicMap)
				model->mMetallicMap->Bind(2);
			else
				model->metallicRGBA->Bind(2);

			if (model->bUseRoughnessMap)
				model->mRoughnessMap->Bind(3);
			else
				model->roughnessRGBA->Bind(3);

			if (model->bUseAoMap)
				model->mAoMap->Bind(4);
			else
				Library<Texture2D>::GetInstance().GetWhiteTexture()->Bind(4);

			shader->SetInt("albedoMap", 0);
			shader->SetInt("normalMap", 1);
			shader->SetInt("metallicMap", 2);
			shader->SetInt("roughnessMap", 3);
			shader->SetInt("aoMap", 4);
			shader->SetFloat3("u_Uniform.camPos", cameraPos);
		}

		RenderCommand::DrawIndexed(mVertexArray, mIB->GetCount());
	}

	void Mesh::Draw()
	{
		//SetupMesh(EntityID);
		static bool bInit = true;
		if (bInit)
		{
			bInit = false;

			mVertexArray->Bind();

			mVB->SetData(mStaticVertices.data(), sizeof(Vertex) * mStaticVertices.size());
			mIB->SetData(mIndices.data(), mIndices.size());

			mVertexArray->Unbind();
		}
		mVertexArray->Bind();
		RenderCommand::DrawIndexed(mVertexArray, mIB->GetCount());
		mVertexArray->Unbind();
	}

	void Mesh::SetupMesh(int entityID)
	{
		if (mEntityID == -1)
		{
			mEntityID = entityID;
			mVertexArray->Bind();

			for (int i = 0; i < mStaticVertices.size(); ++i)
			{
				mStaticVertices[i].EntityID = entityID;
			}

			mVB->SetData(mStaticVertices.data(), sizeof(Vertex) * mStaticVertices.size());

			mIB->SetData(mIndices.data(), mIndices.size());

			mVertexArray->Unbind();
		}
	}
}