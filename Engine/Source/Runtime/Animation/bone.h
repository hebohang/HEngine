#pragma once

/* Container for bone data */

#include <vector>
#include <assimp/scene.h>
#include <list>
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace HEngine
{
	namespace Utils
	{
		static glm::vec3 GetGLMVec(const aiVector3D& vec)
		{
			return glm::vec3(vec.x, vec.y, vec.z);
		}

		static glm::quat GetGLMQuat(const aiQuaternion& pOrientation)
		{
			return glm::quat(pOrientation.w, pOrientation.x, pOrientation.y, pOrientation.z);
		}
	}

	struct KeyPosition
	{
		glm::vec3 position;
		float timeStamp;
	};

	struct KeyRotation
	{
		glm::quat orientation;
		float timeStamp;
	};

	struct KeyScale
	{
		glm::vec3 scale;
		float timeStamp;
	};

	class Bone
	{
	public:
		Bone(const std::string& name, int ID, const aiNodeAnim* channel)
			:
			mName(name),
			mID(ID),
			mLocalTransform(1.0f)
		{
			mNumPositions = channel->mNumPositionKeys;

			for (int positionIndex = 0; positionIndex < mNumPositions; ++positionIndex)
			{
				aiVector3D aiPosition = channel->mPositionKeys[positionIndex].mValue;
				float timeStamp = (float)channel->mPositionKeys[positionIndex].mTime;
				KeyPosition data;
				data.position = Utils::GetGLMVec(aiPosition);
				data.timeStamp = timeStamp;
				mPositions.push_back(data);
			}

			mNumRotations = channel->mNumRotationKeys;
			for (int rotationIndex = 0; rotationIndex < mNumRotations; ++rotationIndex)
			{
				aiQuaternion aiOrientation = channel->mRotationKeys[rotationIndex].mValue;
				float timeStamp = (float)channel->mRotationKeys[rotationIndex].mTime;
				KeyRotation data;
				data.orientation = Utils::GetGLMQuat(aiOrientation);
				data.timeStamp = timeStamp;
				mRotations.push_back(data);
			}

			mNumScalings = channel->mNumScalingKeys;
			for (int keyIndex = 0; keyIndex < mNumScalings; ++keyIndex)
			{
				aiVector3D scale = channel->mScalingKeys[keyIndex].mValue;
				float timeStamp = (float)channel->mScalingKeys[keyIndex].mTime;
				KeyScale data;
				data.scale = Utils::GetGLMVec(scale);
				data.timeStamp = timeStamp;
				mScales.push_back(data);
			}
		}

		void Update(float animationTime)
		{
			glm::mat4 translation = InterpolatePosition(animationTime);
			glm::mat4 rotation = InterpolateRotation(animationTime);
			glm::mat4 scale = InterpolateScaling(animationTime);
			mLocalTransform = translation * rotation * scale;
		}
		glm::mat4 GetLocalTransform() { return mLocalTransform; }
		std::string GetBoneName() const { return mName; }
		int GetBoneID() { return mID; }



		int GetPositionIndex(float animationTime)
		{
			for (int index = 0; index < mNumPositions - 1; ++index)
			{
				if (animationTime < mPositions[index + 1].timeStamp)
					return index;
			}
			assert(0);
		}

		int GetRotationIndex(float animationTime)
		{
			for (int index = 0; index < mNumRotations - 1; ++index)
			{
				if (animationTime < mRotations[index + 1].timeStamp)
					return index;
			}
			assert(0);
		}

		int GetScaleIndex(float animationTime)
		{
			for (int index = 0; index < mNumScalings - 1; ++index)
			{
				if (animationTime < mScales[index + 1].timeStamp)
					return index;
			}
			assert(0);
		}


	private:

		float GetScaleFactor(float lastTimeStamp, float nextTimeStamp, float animationTime)
		{
			float scaleFactor = 0.0f;
			float midWayLength = animationTime - lastTimeStamp;
			float framesDiff = nextTimeStamp - lastTimeStamp;
			scaleFactor = midWayLength / framesDiff;
			return scaleFactor;
		}

		glm::mat4 InterpolatePosition(float animationTime)
		{
			if (1 == mNumPositions)
				return glm::translate(glm::mat4(1.0f), mPositions[0].position);

			int p0Index = GetPositionIndex(animationTime);
			int p1Index = p0Index + 1;
			float scaleFactor = GetScaleFactor(mPositions[p0Index].timeStamp,
				mPositions[p1Index].timeStamp, animationTime);
			glm::vec3 finalPosition = glm::mix(mPositions[p0Index].position, mPositions[p1Index].position
				, scaleFactor);
			return glm::translate(glm::mat4(1.0f), finalPosition);
		}

		glm::mat4 InterpolateRotation(float animationTime)
		{
			if (1 == mNumRotations)
			{
				auto rotation = glm::normalize(mRotations[0].orientation);
				return glm::toMat4(rotation);
			}

			int p0Index = GetRotationIndex(animationTime);
			int p1Index = p0Index + 1;
			float scaleFactor = GetScaleFactor(mRotations[p0Index].timeStamp,
				mRotations[p1Index].timeStamp, animationTime);
			glm::quat finalRotation = glm::slerp(mRotations[p0Index].orientation, mRotations[p1Index].orientation
				, scaleFactor);
			finalRotation = glm::normalize(finalRotation);
			return glm::toMat4(finalRotation);

		}

		glm::mat4 InterpolateScaling(float animationTime)
		{
			if (1 == mNumScalings)
				return glm::scale(glm::mat4(1.0f), mScales[0].scale);

			int p0Index = GetScaleIndex(animationTime);
			int p1Index = p0Index + 1;
			float scaleFactor = GetScaleFactor(mScales[p0Index].timeStamp,
				mScales[p1Index].timeStamp, animationTime);
			glm::vec3 finalScale = glm::mix(mScales[p0Index].scale, mScales[p1Index].scale
				, scaleFactor);
			return glm::scale(glm::mat4(1.0f), finalScale);
		}

		std::vector<KeyPosition> mPositions;
		std::vector<KeyRotation> mRotations;
		std::vector<KeyScale> mScales;
		int mNumPositions;
		int mNumRotations;
		int mNumScalings;

		glm::mat4 mLocalTransform;
		std::string mName;
		int mID;
	};

}
