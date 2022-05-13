#include "hepch.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Renderer/Model.h"
#include "Runtime/Renderer/Mesh.h"

#include <regex>

namespace HEngine 
{
	void Model::Draw(const glm::mat4& transform, const glm::vec3& cameraPos, int entityID)
	{
		for (unsigned int i = 0; i < mMeshes.size(); ++i)
			mMeshes[i].Draw(transform, cameraPos, mMaterial->GetShader(), entityID, this);
	}

	void Model::Draw(const glm::mat4& transform, const glm::vec3& cameraPos, Ref<Shader> shader, int entityID)
	{
		for (unsigned int i = 0; i < mMeshes.size(); ++i)
			mMeshes[i].Draw(transform, cameraPos, shader, entityID, this);
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

		std::string standardPath = std::regex_replace(path, std::regex("\\\\"), "/");
		mDirectory = standardPath.substr(0, standardPath.find_last_of('/'));

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

	Mesh Model::ProcessMesh(aiMesh* mesh, const aiScene* scene)
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

			if (mesh->HasTangentsAndBitangents())
			{
				// tangent
				vector.x = mesh->mTangents[i].x;
				vector.y = mesh->mTangents[i].y;
				vector.z = mesh->mTangents[i].z;
				vertex.Tangent = vector;
				// bitangent
				vector.x = mesh->mBitangents[i].x;
				vector.y = mesh->mBitangents[i].y;
				vector.z = mesh->mBitangents[i].z;
				vertex.Bitangent = vector;
			}
			else
			{
				vertex.Tangent = glm::vec3{ 0.0f };
				vertex.Bitangent = glm::vec3{ 0.0f };
			}

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

		const auto& loadTexture = [&](aiTextureType type) {
			auto maps = loadMaterialTextures(material, type);
			if (maps) textures.insert(textures.end(), maps.value().begin(), maps.value().end());
		};

		for (uint32_t type = aiTextureType_NONE; type < aiTextureType_TRANSMISSION; type++)
		{
			loadTexture(static_cast<aiTextureType>(type));
		}

		return Mesh(vertices, indices, textures);
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
					mAlbedoMap = texture.texture2d;
					break;
				case aiTextureType_SPECULAR:
					texture.type = TextureType::Specular;
					break;
				case aiTextureType_HEIGHT:
					texture.type = TextureType::Height;
					break;
				case aiTextureType_AMBIENT:
					texture.type = TextureType::AmbientOcclusion;
					mAoMap = texture.texture2d;
					break;
				case aiTextureType_BASE_COLOR:
					texture.type = TextureType::Albedo;
					mAlbedoMap = texture.texture2d;
					break;
				case aiTextureType_NORMAL_CAMERA:
					texture.type = TextureType::Normal;
					mNormalMap = texture.texture2d;
					break;
				case aiTextureType_EMISSION_COLOR:
					texture.type = TextureType::Emission;
					break;
				case aiTextureType_METALNESS:
					texture.type = TextureType::Metalness;
					mMetallicMap = texture.texture2d;
					break;
				case aiTextureType_DIFFUSE_ROUGHNESS:
					texture.type = TextureType::Roughness;
					mRoughnessMap = texture.texture2d;
					break;
				case aiTextureType_AMBIENT_OCCLUSION:
					texture.type = TextureType::AmbientOcclusion;
					mAoMap = texture.texture2d;
					break;
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