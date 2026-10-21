#pragma once

/* Container for bone data */

#include "Runtime/Animation/LocalPose.h"

#include <vector>
#include <string>
#if HE_RUNTIME_ASSET_IMPORT
#include <assimp/scene.h>
#endif
#include <list>
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace HEngine
{
	namespace Utils
	{
#if HE_RUNTIME_ASSET_IMPORT
		static glm::vec3 GetGLMVec(const aiVector3D& vec)
		{
			return glm::vec3(vec.x, vec.y, vec.z);
		}

		static glm::quat GetGLMQuat(const aiQuaternion& pOrientation)
		{
			return glm::quat(pOrientation.w, pOrientation.x, pOrientation.y, pOrientation.z);
		}
#endif
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
		// Constructor from raw keyframe vectors (no Assimp dependency)
		Bone(const std::string& name, int ID,
			const std::vector<KeyPosition>& positions,
			const std::vector<KeyRotation>& rotations,
			const std::vector<KeyScale>&    scales)
			: mName(name), mID(ID), mLocalTransform(1.0f),
			  mPositions(positions), mRotations(rotations), mScales(scales),
			  mNumPositions((int)positions.size()),
			  mNumRotations((int)rotations.size()),
			  mNumScalings((int)scales.size())
		{}

#if HE_RUNTIME_ASSET_IMPORT
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
#endif

        LocalPose Sample(float time, const LocalPose& bindPose) const
        {
            LocalPose result = bindPose;
            result.Translation = SampleKeys(mPositions, time, bindPose.Translation,
                [](const KeyPosition& key) { return key.position; },
                [](auto a, auto b, float t) { return glm::mix(a, b, t); });
            result.Rotation = SampleKeys(mRotations, time, bindPose.Rotation,
                [](const KeyRotation& key) { return key.orientation; },
                [](auto a, auto b, float t) { return glm::normalize(glm::slerp(glm::normalize(a), glm::normalize(b), t)); });
            result.Scale = SampleKeys(mScales, time, bindPose.Scale,
                [](const KeyScale& key) { return key.scale; },
                [](auto a, auto b, float t) { return glm::mix(a, b, t); });
            const float rotationLength = glm::length(result.Rotation);
            if (!std::isfinite(rotationLength) || rotationLength < 1e-6f)
                throw std::invalid_argument("Animation key has an invalid quaternion.");
            result.Rotation = glm::normalize(result.Rotation);
            return result;
        }

        void Update(float animationTime) { mLocalTransform = Sample(animationTime, {}).Matrix(); }
        glm::mat4 GetLocalTransform() const { return mLocalTransform; }
        std::string GetBoneName() const { return mName; }
        int GetBoneID() const { return mID; }
        int GetPositionIndex(float time) const { return KeyIndex(mPositions, time); }
        int GetRotationIndex(float time) const { return KeyIndex(mRotations, time); }
        int GetScaleIndex(float time) const { return KeyIndex(mScales, time); }

    private:
        template<class Key>
        static int KeyIndex(const std::vector<Key>& keys, float time)
        {
            if (keys.size() < 2) return 0;
            const auto next = std::upper_bound(keys.begin(), keys.end(), time,
                [](float value, const Key& key) { return value < key.timeStamp; });
            return std::clamp(static_cast<int>(next - keys.begin()) - 1, 0, static_cast<int>(keys.size()) - 2);
        }

        template<class Key, class Value, class Read, class Blend>
        static Value SampleKeys(const std::vector<Key>& keys, float time, Value fallback, Read read, Blend blend)
        {
            if (keys.empty()) return fallback;
            if (keys.size() == 1 || time <= keys.front().timeStamp) return read(keys.front());
            if (time >= keys.back().timeStamp) return read(keys.back());
            const int index = KeyIndex(keys, time);
            const auto& a = keys[index];
            const auto& b = keys[index + 1];
            const float duration = b.timeStamp - a.timeStamp;
            const float alpha = duration > 0.0f ? std::clamp((time - a.timeStamp) / duration, 0.0f, 1.0f) : 1.0f;
            return blend(read(a), read(b), alpha);
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
