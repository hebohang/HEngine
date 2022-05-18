#include "hepch.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Mesh/Mesh.h"
#include "Runtime/Mesh/SubMesh.h"

#include <regex>

namespace HEngine 
{
	namespace Utils
	{
		static void SetVertexBoneDataToDefault(SkinnedVertex& vertex)
		{
			for (int i = 0; i < MAX_BONE_INFLUENCE; i++)
			{
				vertex.mBoneIDs[i] = -1;
				vertex.mWeights[i] = 0.0f;
			}
		}
	}

	void Mesh::Draw(const glm::mat4& transform, const glm::vec3& cameraPos, int entityID)
	{
		for (unsigned int i = 0; i < mSubMeshes.size(); ++i)
			mSubMeshes[i].Draw(transform, cameraPos, mMaterial[0]->GetShader(), entityID, this);
	}

	void Mesh::Draw(const glm::mat4& transform, const glm::vec3& cameraPos, Ref<Shader> shader, int entityID)
	{
		for (unsigned int i = 0; i < mSubMeshes.size(); ++i)
			mSubMeshes[i].Draw(transform, cameraPos, shader, entityID, this);
	}

	void Mesh::Draw()
	{
		for (unsigned int i = 0; i < mSubMeshes.size(); ++i)
			mSubMeshes[i].Draw();
	}

	void Mesh::LoadModel(const std::string& path)
	{
		mMaterial.resize(20);

		Assimp::Importer importer;
		std::string standardPath = std::regex_replace(path, std::regex("\\\\"), "/");
		std::string standardFullPath = std::regex_replace(AssetManager::GetFullPath(path).string(), std::regex("\\\\"), "/");
		const aiScene* scene = importer.ReadFile(standardFullPath, aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace);

		if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
		{
			HE_CORE_ERROR("ERROR::ASSIMP::{0}", importer.GetErrorString());
			return;
		}

		mDirectory = standardPath.substr(0, standardPath.find_last_of('/'));

		uint32_t subMeshIndex = 0;
		if (scene->HasAnimations())
		{
			bAnimated = true;
			ProcessNode(scene->mRootNode, scene, subMeshIndex);
			mAnimation = Animation(standardFullPath, this);
			mAnimator = Animator(&mAnimation);
		}
		else
			ProcessNode(scene->mRootNode, scene, subMeshIndex);

		mMaterial.resize(subMeshIndex);
	}

	void Mesh::ProcessNode(aiNode* node, const aiScene* scene, uint32_t& subMeshIndex)
	{
		for (uint32_t i = 0; i < node->mNumMeshes; ++i)
		{
			aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];

			if (bAnimated)
				mSubMeshes.push_back(ProcessMesh<SkinnedVertex>(mesh, scene, subMeshIndex));
			else
				mSubMeshes.push_back(ProcessMesh<StaticVertex>(mesh, scene, subMeshIndex));

			subMeshIndex++;
		}

		for (uint32_t i = 0; i < node->mNumChildren; ++i)
		{
			ProcessNode(node->mChildren[i], scene, subMeshIndex);
		}
	}

	template <typename Vertex>
	SubMesh Mesh::ProcessMesh(aiMesh* mesh, const aiScene* scene, uint32_t& subMeshIndex)
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<MaterialTexture> textures;

		for (uint32_t i = 0; i < mesh->mNumVertices; ++i)
		{
			Vertex vertex;

			if (bAnimated)
			{
				Utils::SetVertexBoneDataToDefault((*reinterpret_cast<SkinnedVertex*>(&vertex)));
			}

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

		// Bones
		if (bAnimated)
		{
			for (size_t boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex)
			{
				int boneID = -1;
				std::string boneName = mesh->mBones[boneIndex]->mName.C_Str();
				if (mBoneInfoMap.find(boneName) == mBoneInfoMap.end())
				{
					BoneInfo newBoneInfo;
					newBoneInfo.id = mBoneCounter;
					newBoneInfo.offset = Utils::ConvertMatrixToGLMFormat(mesh->mBones[boneIndex]->mOffsetMatrix);
					mBoneInfoMap[boneName] = newBoneInfo;
					boneID = mBoneCounter;
					mBoneCounter++;
				}
				else
				{
					boneID = mBoneInfoMap[boneName].id;
				}
				HE_CORE_ASSERT(boneID != -1);
				auto weights = mesh->mBones[boneIndex]->mWeights;
				int numWeights = mesh->mBones[boneIndex]->mNumWeights;

				for (int weightIndex = 0; weightIndex < numWeights; ++weightIndex)
				{
					int vertexId = weights[weightIndex].mVertexId;
					float weight = weights[weightIndex].mWeight;
					HE_CORE_ASSERT(vertexId <= vertices.size());
					for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
					{
						if ((*reinterpret_cast<SkinnedVertex*>(&vertices[vertexId])).mBoneIDs[i] < 0)
						{
							(*reinterpret_cast<SkinnedVertex*>(&vertices[vertexId])).mWeights[i] = weight;
							(*reinterpret_cast<SkinnedVertex*>(&vertices[vertexId])).mBoneIDs[i] = boneID;
							break;
						}
					}
				}
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

		mMaterial[subMeshIndex] = CreateRef<Material>();

		const auto& loadTexture = [&](aiTextureType type) {
			auto maps = loadMaterialTextures(material, type, subMeshIndex);
			if (maps) textures.insert(textures.end(), maps.value().begin(), maps.value().end());
		};

		for (uint32_t type = aiTextureType_NONE; type < aiTextureType_REFLECTION; type++)
		{
			loadTexture(static_cast<aiTextureType>(type));
		}

		return SubMesh(vertices, indices, textures, subMeshIndex);
	}

	std::optional<std::vector<MaterialTexture>> Mesh::loadMaterialTextures(aiMaterial* mat, aiTextureType type, uint32_t& subMeshIndex)
	{
		std::vector<MaterialTexture> textures;
		for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
		{
			aiString str;
			mat->GetTexture(type, i, &str);

			// check if texture was loaded before and if so, continue to next iteration: skip loading a new texture
			bool skip = false;
			//if (!mMaterial[subMeshIndex]->mTextures.empty())
			//{
			//	for (unsigned int j = 0; j < mMaterial[subMeshIndex]->mTextures.size(); j++)
			//	{
			//		if (std::strcmp(mMaterial[subMeshIndex]->mTextures[j].path.data(), str.C_Str()) == 0)
			//		{
			//			textures.push_back(mMaterial[subMeshIndex]->mTextures[j]);
			//			skip = true; // a texture with the same filepath has already been loaded, continue to next one. (optimization)
			//			break;
			//		}
			//	}
			//}
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
					mMaterial[subMeshIndex]->mAlbedoMap = texture.texture2d;
					break;
				case aiTextureType_SPECULAR:
					texture.type = TextureType::Specular;
					break;
				case aiTextureType_HEIGHT:
					texture.type = TextureType::Height;
					break;
				case aiTextureType_AMBIENT:
					texture.type = TextureType::AmbientOcclusion;
					mMaterial[subMeshIndex]->mAoMap = texture.texture2d;
					break;
				//case aiTextureType_BASE_COLOR:
				//	texture.type = TextureType::Albedo;
				//	mAlbedoMap = texture.texture2d;
				//	break;
				case aiTextureType_NORMALS:
					texture.type = TextureType::Normal;
					mMaterial[subMeshIndex]->mNormalMap = texture.texture2d;
					break;
				case aiTextureType_EMISSIVE:
					texture.type = TextureType::Emission;
					break;
				//case aiTextureType_METALNESS:
				//	texture.type = TextureType::Metalness;
				//	mMetallicMap = texture.texture2d;
				//	break;
				//case aiTextureType_DIFFUSE_ROUGHNESS:
				//	texture.type = TextureType::Roughness;
				//	mRoughnessMap = texture.texture2d;
				//	break;
				//case aiTextureType_AMBIENT_OCCLUSION:
				//	texture.type = TextureType::AmbientOcclusion;
				//	mAoMap = texture.texture2d;
				//	break;
				}
				texture.path = str.C_Str();
				textures.push_back(texture);
				mMaterial[subMeshIndex]->mTextures.push_back(texture);  // store it as texture loaded for entire model, to ensure we won't unnecesery load duplicate textures.
			}
		}

		if (textures.empty()) return {};
		return textures;
	}
}