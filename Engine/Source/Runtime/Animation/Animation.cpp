#include "Hepch.h"

#include "Runtime/Animation/Animation.h"
#include "Runtime/Graphics/Mesh/Mesh.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#if HE_RUNTIME_ASSET_IMPORT
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#endif

#include <glm/gtc/type_ptr.hpp>

namespace HEngine
{
#if HE_RUNTIME_ASSET_IMPORT
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
		RebuildBoneLookup();
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
		dest.bindPose = LocalPose::FromMatrix(dest.transformation);
		dest.childrenCount = src->mNumChildren;

		for (int i = 0; i < src->mNumChildren; i++)
		{
			AssimpNodeData newData;
			ReadHeirarchyData(newData, src->mChildren[i]);
			dest.children.push_back(newData);
		}
	}
#endif

	void Animation::RebuildBoneLookup()
	{
		mBoneNameToIndex.clear();
		mBoneNameToIndex.reserve(mBones.size());

		for (size_t i = 0; i < mBones.size(); ++i)
			mBoneNameToIndex[mBones[i].GetBoneName()] = i;
	}

	void Animation::ReadHierarchyDataFromRes(AssimpNodeData& dest, const AnimNodeRes& src)
	{
		dest.name = src.name;
		if (src.localTransform.size() == 16)
			dest.transformation = glm::make_mat4(src.localTransform.data());
		else
			dest.transformation = glm::mat4(1.0f);
		dest.bindPose = LocalPose::FromMatrix(dest.transformation);

		dest.childrenCount = (int)src.children.size();
		for (const auto& childRes : src.children)
		{
			AssimpNodeData childData;
			ReadHierarchyDataFromRes(childData, childRes);
			dest.children.push_back(childData);
		}
	}

	Animation::Animation(const AnimRes& animRes, int clipIndex, std::map<std::string, BoneInfo>& outBoneInfoMap)
	{
		HE_CORE_ASSERT(clipIndex >= 0 && clipIndex < (int)animRes.clips.size(), "Animation: invalid clip index");

		const AnimClipRes& clip = animRes.clips[clipIndex];
		mDuration       = clip.duration;
		mTicksPerSecond = clip.ticksPerSecond > 0.0f ? clip.ticksPerSecond : 24.0f;
        mEvents = clip.events;
        std::set<std::string> eventIds;
        for (const auto& event : mEvents)
        {
            if (event.id.empty() || event.name.empty() || !eventIds.insert(event.id).second ||
                !std::isfinite(event.timeStamp) || event.timeStamp < 0.0f || event.timeStamp >= mDuration)
                throw std::invalid_argument("Animation events require unique IDs, names and timestamps in [0,duration).");
        }
        std::stable_sort(mEvents.begin(), mEvents.end(), [](const auto& a, const auto& b) { return a.timeStamp < b.timeStamp; });

		ReadHierarchyDataFromRes(mRootNode, animRes.rootNode);

		// Seed from external bone map first (e.g. mesh skeleton resource)
		mBoneInfoMap = outBoneInfoMap;

		// Populate bone info maps
		for (const auto& bi : animRes.boneInfos)
		{
			BoneInfo info;
			info.id = bi.id;
			info.offset = (bi.offsetMatrix.size() == 16) ?
				glm::make_mat4(bi.offsetMatrix.data()) : glm::mat4(1.0f);
			mBoneInfoMap[bi.name]  = info;
			outBoneInfoMap[bi.name] = info;
		}

		// Build Bone objects from channels
		for (const auto& channel : clip.channels)
		{
			int boneId = -1;
			auto it = mBoneInfoMap.find(channel.name);
			if (it != mBoneInfoMap.end())
				boneId = it->second.id;

			std::vector<KeyPosition> positions;
			positions.reserve(channel.positionKeys.size());
			for (const auto& pk : channel.positionKeys)
			{
				KeyPosition kp;
				kp.position  = { pk.px, pk.py, pk.pz };
				kp.timeStamp = pk.timeStamp;
				positions.push_back(kp);
			}

			std::vector<KeyRotation> rotations;
			rotations.reserve(channel.rotationKeys.size());
			for (const auto& rk : channel.rotationKeys)
			{
				KeyRotation kr;
				kr.orientation = glm::quat(rk.qw, rk.qx, rk.qy, rk.qz);
				kr.timeStamp   = rk.timeStamp;
				rotations.push_back(kr);
			}

			std::vector<KeyScale> scales;
			scales.reserve(channel.scaleKeys.size());
			for (const auto& sk : channel.scaleKeys)
			{
				KeyScale ks;
				ks.scale     = { sk.sx, sk.sy, sk.sz };
				ks.timeStamp = sk.timeStamp;
				scales.push_back(ks);
			}

			mBones.emplace_back(channel.name, boneId, positions, rotations, scales);
		}

		RebuildBoneLookup();
	}
}
