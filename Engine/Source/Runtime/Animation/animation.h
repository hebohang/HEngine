#pragma once

#include "Runtime/Animation/animdata.h"
#include "Runtime/Animation/bone.h"

#include <vector>
#include <map>
#include <glm/glm.hpp>
#include <assimp/scene.h>
#include <functional>

namespace HEngine
{
	namespace Utils
	{
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
	}

	struct AssimpNodeData
	{
		glm::mat4 transformation;
		std::string name;
		int childrenCount;
		std::vector<AssimpNodeData> children;
	};

	class Animation
	{
	public:
		Animation() = default;
		Animation(const Animation&) = default;

		Animation(const std::string& animationPath, class Model* model);

		~Animation()
		{
		}

		Bone* FindBone(const std::string& name)
		{
			auto iter = std::find_if(mBones.begin(), mBones.end(),
				[&](const Bone& Bone)
				{
					return Bone.GetBoneName() == name;
				}
			);
			if (iter == mBones.end()) return nullptr;
			else return &(*iter);
		}


		inline float GetTicksPerSecond() { return mTicksPerSecond; }
		inline float GetDuration() { return mDuration; }
		inline const AssimpNodeData& GetRootNode() { return mRootNode; }
		inline const std::map<std::string, BoneInfo>& GetBoneIDMap()
		{
			return mBoneInfoMap;
		}

	private:
		// important
		void ReadMissingBones(const aiAnimation* animation, class Model& model);

		void ReadHeirarchyData(AssimpNodeData& dest, const aiNode* src);

		float mDuration;
		int mTicksPerSecond;
		std::vector<Bone> mBones;
		AssimpNodeData mRootNode;
		std::map<std::string, BoneInfo> mBoneInfoMap;
	};
}
