#pragma once

#include "Runtime/Animation/animation.h"
#include "Runtime/Animation/bone.h"

#include <glm/glm.hpp>
#include <map>
#include <vector>
#include <assimp/scene.h>
#include <assimp/Importer.hpp>


namespace HEngine
{
	class Animator
	{
	public:
		Animator() = default;
		Animator(const Animator&) = default;

		Animator(Animation* animation)
		{
			mCurrentTime = 0.0;
			mCurrentAnimation = animation;

			mFinalBoneMatrices.reserve(100);

			for (int i = 0; i < 100; i++)
				mFinalBoneMatrices.push_back(glm::mat4(1.0f));
		}

		void UpdateAnimation(float dt)
		{
			mDeltaTime = dt;
			if (mCurrentAnimation)
			{
				mCurrentTime += mCurrentAnimation->GetTicksPerSecond() * dt;
				mCurrentTime = fmod(mCurrentTime, mCurrentAnimation->GetDuration());
				CalculateBoneTransform(&mCurrentAnimation->GetRootNode(), glm::mat4(1.0f));
			}
		}

		void PlayAnimation(Animation* pAnimation)
		{
			mCurrentAnimation = pAnimation;
			mCurrentTime = 0.0f;
		}

		void CalculateBoneTransform(const AssimpNodeData* node, glm::mat4 parentTransform)
		{
			std::string nodeName = node->name;
			glm::mat4 nodeTransform = node->transformation;

			Bone* Bone = mCurrentAnimation->FindBone(nodeName);

			if (Bone)
			{
				Bone->Update(mCurrentTime);
				nodeTransform = Bone->GetLocalTransform();
			}

			glm::mat4 globalTransformation = parentTransform * nodeTransform;

			auto boneInfoMap = mCurrentAnimation->GetBoneIDMap();
			if (boneInfoMap.find(nodeName) != boneInfoMap.end())
			{
				int index = boneInfoMap[nodeName].id;
				glm::mat4 offset = boneInfoMap[nodeName].offset;
				mFinalBoneMatrices[index] = globalTransformation * offset;
			}

			for (int i = 0; i < node->childrenCount; i++)
				CalculateBoneTransform(&node->children[i], globalTransformation);
		}

		std::vector<glm::mat4> GetFinalBoneMatrices()
		{
			return mFinalBoneMatrices;
		}

	private:
		std::vector<glm::mat4> mFinalBoneMatrices;
		Animation* mCurrentAnimation;
		float mCurrentTime;
		float mDeltaTime;

	};
}
