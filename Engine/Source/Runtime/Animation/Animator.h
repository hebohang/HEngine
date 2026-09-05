#pragma once

#include "Runtime/Animation/Animation.h"
#include "Runtime/Animation/Bone.h"

#include <glm/glm.hpp>
#include <map>
#include <vector>


namespace HEngine
{
	class Animator
	{
	public:
		Animator()
		{
			mFinalBoneMatrices.assign(100, glm::mat4(1.0f));
		}
		Animator(const Animator&) = default;

		Animator(Animation* animation)
		{
			mCurrentTime = 0.0f;
			mCurrentAnimation = animation;
			mFinalBoneMatrices.assign(100, glm::mat4(1.0f));
		}

		void UpdateAnimation(float dt)
		{
			mDeltaTime = dt;
			if (mCurrentAnimation)
			{
				mCurrentTime += mCurrentAnimation->GetTicksPerSecond() * dt;
				if (mCurrentAnimation->GetDuration() > 0.0f)
					mCurrentTime = fmod(mCurrentTime, mCurrentAnimation->GetDuration());
				const AssimpNodeData& rootNode = mCurrentAnimation->GetRootNode();
				if (rootNode.name.empty() && rootNode.childrenCount == 0 && rootNode.children.empty())
					return;
				CalculateBoneTransform(&rootNode, glm::mat4(1.0f));
			}
		}

		void UpdateAnimationAbsolute(float time)
		{
			if (!mCurrentAnimation)
				return;

			if (mCurrentAnimation->GetDuration() > 0.0f)
				mCurrentTime = fmod(time, mCurrentAnimation->GetDuration());
			else
				mCurrentTime = 0.0f;

			const AssimpNodeData& rootNode = mCurrentAnimation->GetRootNode();
			if (rootNode.name.empty() && rootNode.childrenCount == 0 && rootNode.children.empty())
				return;
			CalculateBoneTransform(&rootNode, glm::mat4(1.0f));
		}

		void PlayAnimation(Animation* pAnimation)
		{
			mCurrentAnimation = pAnimation;
			mCurrentTime = 0.0f;
		}

		void SetRootMotionOptions(bool bEnableRootMotion, bool bLockRootMotionXZ)
		{
			mEnableRootMotion = bEnableRootMotion;
			mLockRootMotionXZ = bLockRootMotionXZ;
		}

		void CalculateBoneTransform(const AssimpNodeData* node, glm::mat4 parentTransform)
		{
			if (!node || !mCurrentAnimation)
				return;

			std::string nodeName = node->name;
			glm::mat4 nodeTransform = node->transformation;

			Bone* Bone = mCurrentAnimation->FindBone(nodeName);

			if (Bone)
			{
				Bone->Update(mCurrentTime);
				nodeTransform = Bone->GetLocalTransform();

				if (!mEnableRootMotion)
				{
					const bool bRootTranslationNode =
						nodeName.find("Hips_$AssimpFbx$_Translation") != std::string::npos ||
						nodeName == "Hips_$AssimpFbx$_Translation" ||
						nodeName == "mixamorig:Hips_$AssimpFbx$_Translation";

					if (bRootTranslationNode)
					{
						if (mLockRootMotionXZ)
						{
							nodeTransform[3][0] = node->transformation[3][0];
							nodeTransform[3][2] = node->transformation[3][2];
						}
						else
						{
							nodeTransform[3][0] = node->transformation[3][0];
							nodeTransform[3][1] = node->transformation[3][1];
							nodeTransform[3][2] = node->transformation[3][2];
						}
					}
				}
			}

			glm::mat4 globalTransformation = parentTransform * nodeTransform;

			const auto& boneInfoMap = mCurrentAnimation->GetBoneIDMap();
			auto iter = boneInfoMap.find(nodeName);
			if (iter != boneInfoMap.end())
			{
				int index = iter->second.id;
				glm::mat4 offset = iter->second.offset;
				mFinalBoneMatrices[index] = globalTransformation * offset;
			}

			for (int i = 0; i < node->childrenCount; i++)
				CalculateBoneTransform(&node->children[i], globalTransformation);
		}

		const std::vector<glm::mat4>& GetFinalBoneMatrices() const
		{
			return mFinalBoneMatrices;
		}

		float GetProgress()
		{
			if (!mCurrentAnimation || mCurrentAnimation->GetDuration() <= 0.0f)
				return 0.0f;
			return mCurrentTime / mCurrentAnimation->GetDuration();
		}

		float GetCurrentTime() const { return mCurrentTime; }

		void Reset()
		{
			mCurrentTime = 0.0f;
		}

		void ClearAnimation()
		{
			mCurrentAnimation = nullptr;
			mCurrentTime = 0.0f;
			mDeltaTime = 0.0f;
			mFinalBoneMatrices.assign(100, glm::mat4(1.0f));
		}

	private:
		std::vector<glm::mat4> mFinalBoneMatrices;
		Animation* mCurrentAnimation = nullptr;
		float mCurrentTime = 0.0f;
		float mDeltaTime = 0.0f;
		bool mEnableRootMotion = false;
		bool mLockRootMotionXZ = true;

	};
}
