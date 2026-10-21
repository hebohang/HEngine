#pragma once

#include "Runtime/Animation/Animdata.h"
#include "Runtime/Animation/Bone.h"
#include "Runtime/Resource/ResType/Animation.h"

#include <vector>
#include <map>
#include <unordered_map>
#include <glm/glm.hpp>
#if HE_RUNTIME_ASSET_IMPORT
#include <assimp/scene.h>
#endif
#include <functional>

namespace HEngine
{
	namespace Utils
	{
#if HE_RUNTIME_ASSET_IMPORT
		static glm::mat4 ConvertMatrixToGLMFormat(const aiMatrix4x4& from)
		{
			glm::mat4 to;
			//the a,b,c,d in assimp is the row ; the 1,2,3,4 is the column
			to[0][0] = from.a1; to[1][0] = from.a2; to[2][0] = from.a3; to[3][0] = from.a4;
			to[0][1] = from.b1; to[1][1] = from.b2; to[2][1] = from.b3; to[3][1] = from.b4;
			to[0][2] = from.c1; to[1][2] = from.c2; to[2][2] = from.c3; to[3][2] = from.c4;
			to[0][3] = from.d1; to[1][3] = from.d2; to[2][3] = from.d3; to[3][3] = from.d4;
			return to;
		}
#endif
	}

	struct AssimpNodeData
	{
		glm::mat4 transformation = glm::mat4(1.0f);
		LocalPose bindPose;
		std::string name;
		int childrenCount = 0;
		std::vector<AssimpNodeData> children;
	};

	class Animation
	{
	public:
		Animation() = default;
		Animation(const Animation&) = default;

#if HE_RUNTIME_ASSET_IMPORT
		Animation(const std::string& animationPath, class Mesh* model);
#endif

		// Construct from a serialised AnimRes clip (no Assimp dependency)
		Animation(const AnimRes& animRes, int clipIndex, std::map<std::string, BoneInfo>& outBoneInfoMap);

		~Animation()
		{
		}

		Bone* FindBone(const std::string& name)
		{
			auto iter = mBoneNameToIndex.find(name);
			if (iter == mBoneNameToIndex.end())
				return nullptr;

			return &mBones[iter->second];
		}


		inline const std::vector<AnimEventRes>& GetEvents() const { return mEvents; }
		inline float GetTicksPerSecond() { return mTicksPerSecond; }
		inline float GetDuration() { return mDuration; }
		inline const AssimpNodeData& GetRootNode() const { return mRootNode; }
		inline const std::map<std::string, BoneInfo>& GetBoneIDMap() const
		{
			return mBoneInfoMap;
		}

	private:
		// important
#if HE_RUNTIME_ASSET_IMPORT
		void ReadMissingBones(const aiAnimation* animation, class Mesh& model);

		void ReadHeirarchyData(AssimpNodeData& dest, const aiNode* src);
#endif

		void ReadHierarchyDataFromRes(AssimpNodeData& dest, const AnimNodeRes& src);

		void RebuildBoneLookup();

		float mDuration = 0.0f;
		float mTicksPerSecond = 24.0f;
        std::vector<AnimEventRes> mEvents;
		std::vector<Bone> mBones;
		AssimpNodeData mRootNode;
		std::map<std::string, BoneInfo> mBoneInfoMap;
		std::unordered_map<std::string, size_t> mBoneNameToIndex;
	};
}
