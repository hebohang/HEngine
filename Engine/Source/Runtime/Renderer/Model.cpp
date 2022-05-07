#include "hepch.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Renderer/Model.h"

namespace HEngine 
{
	void Model::Draw(const glm::mat4& transform, int entityID)
	{
		for (unsigned int i = 0; i < mMeshes.size(); ++i)
			mMeshes[i].Draw(transform, mMaterial->GetShader(), entityID);
	}

	void Model::Draw(const glm::mat4& transform, Ref<Shader> shader, int entityID)
	{
		for (unsigned int i = 0; i < mMeshes.size(); ++i)
			mMeshes[i].Draw(transform, shader, entityID);
	}

	void Model::Draw()
	{
		for (unsigned int i = 0; i < mMeshes.size(); ++i)
			mMeshes[i].Draw();
	}

	void Model::LoadModel(const std::string& path)
	{
		Assimp::Importer importer;
		const aiScene* scene = importer.ReadFile(AssetManager::GetFullPath(path).string(), aiProcess_Triangulate | aiProcess_FlipUVs);

		if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
		{
			HE_CORE_ASSERT("ERROR::ASSIMP::{0}", importer.GetErrorString());
			return;
		}

		mDirectory = path.substr(0, path.find_last_not_of('/') + 1);

		ProcessNode(scene->mRootNode, scene);
	}

	void Model::ProcessNode(aiNode* node, const aiScene* scene)
	{
		for (uint32_t i = 0; i < node->mNumMeshes; ++i)
		{
			aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
			mMeshes.push_back(ProcessMesh(mesh, scene));
		}

		for (uint32_t i = 0; i < node->mNumChildren; ++i)
		{
			ProcessNode(node->mChildren[i], scene);
		}
	}

	StaticMesh Model::ProcessMesh(aiMesh* mesh, const aiScene* scene)
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<MaterialTexture> textures;

		for (uint32_t i = 0; i < mesh->mNumVertices; ++i)
		{
			Vertex vertex;

			//pos
			glm::vec3 vector;
			vector.x = mesh->mVertices[i].x;
			vector.y = mesh->mVertices[i].y;
			vector.z = mesh->mVertices[i].z;
			vertex.Pos = vector;

			//normal
			if (mesh->HasNormals())
			{
				vector.x = mesh->mNormals[i].x;
				vector.y = mesh->mNormals[i].y;
				vector.z = mesh->mNormals[i].z;
				vertex.Normal = vector;
			}

			//tangent
			//vector.x = mesh->mTangents[i].x;
			//vector.y = mesh->mTangents[i].y;
			//vector.z = mesh->mTangents[i].z;
			//vertex.Tangent = vector;

			//tex coord
			if (mesh->mTextureCoords[0])
			{
				glm::vec2 vec;
				vec.x = mesh->mTextureCoords[0][i].x;
				vec.y = mesh->mTextureCoords[0][i].y;
				vertex.TexCoord = vec;
			}
			else
				vertex.TexCoord = glm::vec2(0.0f, 0.0f);

			vertex.EntityID = -1;

			vertices.push_back(vertex);
		}

		for (uint32_t i = 0; i < mesh->mNumFaces; ++i)
		{
			aiFace face = mesh->mFaces[i];
			for (uint32_t j = 0; j < face.mNumIndices; ++j)
			{
				indices.push_back(face.mIndices[j]);
			}
		}

		// process materials
		aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
		// we assume a convention for sampler names in the shaders. Each diffuse texture should be named
		// as 'texture_diffuseN' where N is a sequential number ranging from 1 to MAX_SAMPLER_NUMBER. 
		// Same applies to other texture as the following list summarizes:
		// diffuse: texture_diffuseN
		// specular: texture_specularN
		// normal: texture_normalN

		// 1. diffuse maps
		auto diffuseMaps = loadMaterialTextures(material, aiTextureType_DIFFUSE);
		if (diffuseMaps) textures.insert(textures.end(), diffuseMaps.value().begin(), diffuseMaps.value().end());
		// 2. specular maps
		auto specularMaps = loadMaterialTextures(material, aiTextureType_SPECULAR);
		if (specularMaps) textures.insert(textures.end(), specularMaps.value().begin(), specularMaps.value().end());
		// 3. normal maps
		auto normalMaps = loadMaterialTextures(material, aiTextureType_HEIGHT);
		if (normalMaps) textures.insert(textures.end(), normalMaps.value().begin(), normalMaps.value().end());
		// 4. height maps
		auto heightMaps = loadMaterialTextures(material, aiTextureType_AMBIENT);
		if (heightMaps) textures.insert(textures.end(), heightMaps.value().begin(), heightMaps.value().end());

		return StaticMesh(vertices, indices, textures);
	}

	std::optional<std::vector<MaterialTexture>> Model::loadMaterialTextures(aiMaterial* mat, aiTextureType type)
	{
		std::vector<MaterialTexture> textures;
		for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
		{
			aiString str;
			mat->GetTexture(type, i, &str);

			// check if texture was loaded before and if so, continue to next iteration: skip loading a new texture
			bool skip = false;
			for (unsigned int j = 0; j < mMaterial->mTextures.size(); j++)
			{
				if (std::strcmp(mMaterial->mTextures[j].path.data(), str.C_Str()) == 0)
				{
					textures.push_back(mMaterial->mTextures[j]);
					skip = true; // a texture with the same filepath has already been loaded, continue to next one. (optimization)
					break;
				}
			}
			if (!skip)
			{   // if texture hasn't been loaded already, load it
				MaterialTexture texture;

				std::string filename = std::string(str.C_Str());
				filename = mDirectory + '/' + filename;
				try
				{
					texture.texture2d = Texture2D::Create(filename);
				}
				catch (...)
				{
					HE_CORE_WARN("Load Texture failed!");
					texture.texture2d = Texture2D::Create(AssetManager::GetFullPath("Assets/Textures/DefaultTexture.png"));
				}

				switch (type)
				{
				case aiTextureType_DIFFUSE:
					texture.type = TextureType::Albedo;
				case aiTextureType_SPECULAR:
					texture.type = TextureType::Specular;
				case aiTextureType_HEIGHT:
					texture.type = TextureType::Height;
				case aiTextureType_AMBIENT:
					texture.type = TextureType::AmbientOcclusion;
				}
				texture.path = str.C_Str();
				textures.push_back(texture);
				mMaterial->mTextures.push_back(texture);  // store it as texture loaded for entire model, to ensure we won't unnecesery load duplicate textures.
			}
		}

		if (textures.empty()) return {};
		return textures;
	}
}