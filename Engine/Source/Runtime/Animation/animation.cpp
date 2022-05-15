#include "hepch.h"

#include "Runtime/Animation/animation.h"
#include "Runtime/Mesh/Mesh.h"

//#include <assimp/cimport.h>

namespace HEngine
{
	Animation::Animation(const std::string& animationPath, Mesh* model)
	{
		Assimp::Importer importer;
		
		// from https://github.com/assimp/assimp/issues/2544
		//unsigned int ppsteps = aiProcess_LimitBoneWeights | aiProcess_Triangulate | aiProcess_GenSmoothNormals;
		//aiPropertyStore* props = aiCreatePropertyStore();
		//aiSetImportPropertyInteger(props, AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_LINE | aiPrimitiveType_POINT);
		//aiSetImportPropertyInteger(props, AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0);
		//aiSetImportPropertyInteger(props, AI_CONFIG_IMPORT_REMOVE_EMPTY_BONES, 0);
		////auto scene = (aiScene*)aiImportFileExWithProperties(assetPath, ppsteps, NULL, props);
		//const aiScene* scene = aiImportFileExWithProperties(animationPath.c_str(), ppsteps, NULL, props);
		//aiReleasePropertyStore(props);

		static const uint32_t s_MeshImportFlags =
			aiProcess_CalcTangentSpace |        // Create binormals/tangents just in case
			aiProcess_Triangulate |             // Make sure we're triangles
			aiProcess_SortByPType |             // Split meshes by primitive type
			aiProcess_GenNormals |              // Make sure we have legit normals
			aiProcess_GenUVCoords |             // Convert UVs if required 
			//aiProcess_OptimizeMeshes |          // Batch draws where possible
			aiProcess_ValidateDataStructure;    // Validation

		const aiScene* scene = importer.ReadFile(animationPath, s_MeshImportFlags);
		assert(scene && scene->mRootNode);
		auto animation = scene->mAnimations[0];
		mDuration = animation->mDuration;
		mTicksPerSecond = animation->mTicksPerSecond;
		aiMatrix4x4 globalTransformation = scene->mRootNode->mTransformation;
		globalTransformation = globalTransformation.Inverse();
		ReadHeirarchyData(mRootNode, scene->mRootNode);
		ReadMissingBones(animation, *model);
	}
	
	void Animation::ReadMissingBones(const aiAnimation* animation, Mesh& model)
	{
		int size = animation->mNumChannels;

		auto& boneInfoMap = model.GetBoneInfoMap();//getting mBoneInfoMap from Mesh class
		int& boneCount = model.GetBoneCount(); //getting the mBoneCounter from Mesh class

		//reading channels(bones engaged in an animation and their keyframes)
		for (int i = 0; i < size; i++)
		{
			auto channel = animation->mChannels[i];
			std::string boneName = channel->mNodeName.data;

			if (boneInfoMap.find(boneName) == boneInfoMap.end())
			{
				boneInfoMap[boneName].id = boneCount;
				boneCount++;
			}
			mBones.push_back(Bone(channel->mNodeName.data,
				boneInfoMap[channel->mNodeName.data].id, channel));
		}

		mBoneInfoMap = boneInfoMap;
	}

	void Animation::ReadHeirarchyData(AssimpNodeData& dest, const aiNode* src)
	{
		assert(src);

		dest.name = src->mName.data;
		dest.transformation = Utils::ConvertMatrixToGLMFormat(src->mTransformation);
		dest.childrenCount = src->mNumChildren;

		for (int i = 0; i < src->mNumChildren; i++)
		{
			AssimpNodeData newData;
			ReadHeirarchyData(newData, src->mChildren[i]);
			dest.children.push_back(newData);
		}
	}
}