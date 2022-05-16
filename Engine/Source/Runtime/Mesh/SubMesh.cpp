#include "hepch.h"

#include "Runtime/Mesh/SubMesh.h"
#include "Runtime/Renderer/RenderCommand.h"
#include "Runtime/Library/TextureLibrary.h"
#include "Runtime/Mesh/Mesh.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

namespace HEngine
{
	SubMesh::SubMesh(const std::vector<StaticVertex>& vertices, const std::vector<uint32_t> indices)
		: mStaticVertices(vertices), mIndices(indices)
	{
		mVertexArray = VertexArray::Create();

		mVB = VertexBuffer::Create(sizeof(StaticVertex) * vertices.size());
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

	SubMesh::SubMesh(const std::vector<StaticVertex>& vertices, const std::vector<uint32_t> indices, const std::vector<MaterialTexture>& textures)
		: mStaticVertices(vertices), mIndices(indices), mTextures(textures)
	{
		mVertexArray = VertexArray::Create();

		mVB = VertexBuffer::Create(sizeof(StaticVertex) * vertices.size());
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

	SubMesh::SubMesh(const std::vector<SkinnedVertex>& vertices, const std::vector<uint32_t> indices)
		: mSkinnedVertices(vertices), mIndices(indices)
	{
		mVertexArray = VertexArray::Create();

		mVB = VertexBuffer::Create(sizeof(SkinnedVertex) * vertices.size());
		mVB->SetLayout({
					{ ShaderDataType::Float3, "a_Pos"},
					{ ShaderDataType::Float3, "a_Normal"},
					{ ShaderDataType::Float2, "a_TexCoord"},
					{ ShaderDataType::Float3, "a_Tangent"},
					{ ShaderDataType::Float3, "a_Bitangent"},
					{ ShaderDataType::Int,	  "a_EntityID"},
					{ ShaderDataType::Int4,   "a_BoneIDs"},
					{ ShaderDataType::Float4, "a_Weights"},
			});

		mVertexArray->AddVertexBuffer(mVB);

		mIB = IndexBuffer::Create(indices.size());

		mVertexArray->SetIndexBuffer(mIB);
	}

	SubMesh::SubMesh(const std::vector<SkinnedVertex>& vertices, const std::vector<uint32_t> indices, const std::vector<MaterialTexture>& textures)
		: mSkinnedVertices(vertices), mIndices(indices), mTextures(textures)
	{
		mVertexArray = VertexArray::Create();

		mVB = VertexBuffer::Create(sizeof(SkinnedVertex) * vertices.size());
		mVB->SetLayout({
					{ ShaderDataType::Float3, "a_Pos"},
					{ ShaderDataType::Float3, "a_Normal"},
					{ ShaderDataType::Float2, "a_TexCoord"},
					{ ShaderDataType::Float3, "a_Tangent"},
					{ ShaderDataType::Float3, "a_Bitangent"},
					{ ShaderDataType::Int,	  "a_EntityID"},
					{ ShaderDataType::Int4,   "a_BoneIDs"},
					{ ShaderDataType::Float4, "a_Weights"},
			});

		mVertexArray->AddVertexBuffer(mVB);

		mIB = IndexBuffer::Create(indices.size());

		mVertexArray->SetIndexBuffer(mIB);
	}

	void SubMesh::Draw(const glm::mat4& transform, const glm::vec3& cameraPos, const Ref<Shader>& shader, int entityID, Mesh* model)
	{
		SetupMesh(entityID);

		shader->Bind();
		if (model->bPlayAnim)
		{
			model->mAnimator.UpdateAnimation(0.01f * model->mAnimPlaySpeed);

			auto transforms = model->mAnimator.GetFinalBoneMatrices();
			for (int i = 0; i < transforms.size(); ++i)
				shader->SetMat4("finalBonesMatrices[" + std::to_string(i) + "]", transforms[i]);
		}

		if (ModeManager::bHdrUse)
		{
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
			shader->SetMat4("u_Model.Transform", (transform)); // for static 
			shader->SetMat4("model", transform); // for animation 
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

	void SubMesh::Draw()
	{
		//SetupMesh(EntityID);
		static bool bInit = true;
		if (bInit)
		{
			bInit = false;

			mVertexArray->Bind();
			
			if (mStaticVertices.empty())
				mVB->SetData(mSkinnedVertices.data(), sizeof(StaticVertex) * mSkinnedVertices.size());
			else
				mVB->SetData(mStaticVertices.data(), sizeof(StaticVertex) * mStaticVertices.size());
			mIB->SetData(mIndices.data(), mIndices.size());

			mVertexArray->Unbind();
		}
		mVertexArray->Bind();
		RenderCommand::DrawIndexed(mVertexArray, mIB->GetCount());
		mVertexArray->Unbind();
	}

	void SubMesh::SetupMesh(int entityID)
	{
		if (mEntityID == -1)
		{
			mEntityID = entityID;
			mVertexArray->Bind();

			if (mStaticVertices.empty())
			{
				for (int i = 0; i < mSkinnedVertices.size(); ++i)
				{
					mSkinnedVertices[i].EntityID = entityID;
				}

				mVB->SetData(mSkinnedVertices.data(), sizeof(SkinnedVertex) * mSkinnedVertices.size());
			}
			else
			{
				for (int i = 0; i < mStaticVertices.size(); ++i)
				{
					mStaticVertices[i].EntityID = entityID;
				}

				mVB->SetData(mStaticVertices.data(), sizeof(StaticVertex) * mStaticVertices.size());
			}

			mIB->SetData(mIndices.data(), mIndices.size());

			mVertexArray->Unbind();
		}
	}
}