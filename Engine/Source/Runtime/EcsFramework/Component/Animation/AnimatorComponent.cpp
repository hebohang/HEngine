#include "Hepch.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"

#include "Runtime/Animation/SkeletonCompatibility.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    void AnimatorComponent::RebindCompatibleMesh(Mesh& previous, Mesh& replacement)
    {
        if (&previous == &replacement) return;
        const auto incompatibility = GetSkinPaletteIncompatibility(
            replacement.GetBoneInfoMap(), previous.GetBoneInfoMap());
        if (!incompatibility.empty()) throw std::invalid_argument(incompatibility);

        // The controller and its transition animators belong to this component,
        // not to an outfit. Only mesh-owned playback storage needs rebinding.
        replacement.EnsureUniqueDeformation();
        replacement.mAnimation = previous.mAnimation;
        replacement.mAnimator = previous.mAnimator;
        replacement.mAnimator.RebindAnimationStorage(previous.mAnimation, replacement.mAnimation);
        replacement.GetBoneInfoMap() = previous.GetBoneInfoMap();
        replacement.GetBoneCount() = previous.GetBoneCount();
        replacement.mSkeletonPath = previous.mSkeletonPath;
        replacement.bAnimated = previous.bAnimated;
        replacement.bPlayAnim = previous.bPlayAnim;
        replacement.bStopAnim = previous.bStopAnim;
        replacement.mAnimPlaySpeed = previous.mAnimPlaySpeed;
        replacement.bUseExternalBoneMatrices = previous.bUseExternalBoneMatrices;
        replacement.mExternalBoneMatrices = previous.mExternalBoneMatrices;
        for (const auto& section : replacement.mSubMeshes)
        {
            if (!section) continue;
            section->BeginDeformation();
            section->ApplyMorphWeights(FacialWeights);
            section->EndDeformation(true);
        }
        mTransitionFromAnimator.RebindAnimationStorage(previous.mAnimation, replacement.mAnimation);
        mTransitionToAnimator.RebindAnimationStorage(previous.mAnimation, replacement.mAnimation);
        mSecondaryMotion.Reset();
        mCharacterPose.Reset();
        mClothSimulation.clear();
        mDeformationWasActive = false;
    }

    int AnimatorComponent::GetCurrentClipIndex() const
    {
        if (mCurrentNodeId >= 0)
            if (const auto it = mNodeToClipIndex.find(mCurrentNodeId); it != mNodeToClipIndex.end())
                return it->second;
        return currentClipIndex;
    }

    void AnimatorComponent::Play(int clipIndex)
    {
        if (clipIndex >= 0)
            currentClipIndex = clipIndex;

        bPlay = true;
        bPaused = false;
        bHoldFinalPose = false;
        mLastClipIndex = -1;
        mInTransition = false;
        mStepSeconds = 0.0f;
        mAnimationEvents = Json::array();
    }

    void AnimatorComponent::Stop()
    {
        bPlay = false;
        mProgress = 0.0f;
        bPaused = false;
        bHoldFinalPose = false;
        mStepSeconds = 0.0f;
        mLastClipIndex = -1;
        mInTransition = false;
        mAnimationEvents = Json::array();
        mTransitionFromAnimator.ClearAnimation();
        mTransitionToAnimator.ClearAnimation();
    }

    void AnimatorComponent::Pause() { bPaused = true; }
    void AnimatorComponent::Resume() { bPaused = false; mStepSeconds = 0.0f; }
    void AnimatorComponent::Step(float seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0.0f || seconds > 60.0f || mStepSeconds + seconds > 60.0f)
            throw std::invalid_argument("Animation step must accumulate at most 60 nonnegative seconds.");
        bPaused = true;
        mStepSeconds += seconds;
    }
    Json AnimatorComponent::ConsumeEvents() { return std::exchange(mAnimationEvents, Json::array()); }

    void AnimatorComponent::SetSpeed(float speed)
    {
        if (!std::isfinite(speed) || speed < 0.0f)
            throw std::invalid_argument("Animation speed must be finite and nonnegative.");
        playSpeed = speed;
    }

    void AnimatorComponent::SetBool(const std::string& name, bool value)
    {
        if (!mParameterTypes.count(name) || mParameterTypes[name] != AnimControllerParamType::Bool)
        {
            HE_CORE_WARN("AnimatorComponent::SetBool parameter not found or wrong type: {0}", name);
            return;
        }
        mParameters[name] = value;
    }

    bool AnimatorComponent::IsParameterType(const std::string& name, AnimControllerParamType type) const
    {
        const auto parameter = mParameterTypes.find(name);
        return parameter != mParameterTypes.end() && parameter->second == type;
    }

    void AnimatorComponent::SetInt(const std::string& name, int value)
    {
        if (!mParameterTypes.count(name) || mParameterTypes[name] != AnimControllerParamType::Int)
        {
            HE_CORE_WARN("AnimatorComponent::SetInt parameter not found or wrong type: {0}", name);
            return;
        }
        mParameters[name] = value;
    }

    void AnimatorComponent::SetFloat(const std::string& name, float value)
    {
        if (!std::isfinite(value))
            throw std::invalid_argument("Animation float parameter must be finite.");
        if (!mParameterTypes.count(name) || mParameterTypes[name] != AnimControllerParamType::Float)
        {
            HE_CORE_WARN("AnimatorComponent::SetFloat parameter not found or wrong type: {0}", name);
            return;
        }
        mParameters[name] = value;
    }

    void AnimatorComponent::SetTrigger(const std::string& name)
    {
        if (!mParameterTypes.count(name) || mParameterTypes[name] != AnimControllerParamType::Trigger)
        {
            HE_CORE_WARN("AnimatorComponent::SetTrigger parameter not found or wrong type: {0}", name);
            return;
        }
        mParameters[name] = true;
    }

    void AnimatorComponent::ResetTrigger(const std::string& name)
    {
        if (!mParameterTypes.count(name) || mParameterTypes[name] != AnimControllerParamType::Trigger)
            return;
        mParameters[name] = false;
    }

    bool AnimatorComponent::HasParameter(const std::string& name) const
    {
        return mParameters.count(name) > 0;
    }

    bool AnimatorComponent::GetBool(const std::string& name, bool defaultValue) const
    {
        auto iter = mParameters.find(name);
        if (iter == mParameters.end() || !iter->second.is_boolean())
            return defaultValue;
        return iter->second.get<bool>();
    }

    int AnimatorComponent::GetInt(const std::string& name, int defaultValue) const
    {
        auto iter = mParameters.find(name);
        if (iter == mParameters.end() || !iter->second.is_number_integer())
            return defaultValue;
        return iter->second.get<int>();
    }

    float AnimatorComponent::GetFloat(const std::string& name, float defaultValue) const
    {
        auto iter = mParameters.find(name);
        if (iter == mParameters.end() || !iter->second.is_number())
            return defaultValue;
        return iter->second.get<float>();
    }

    bool AnimatorComponent::GetTrigger(const std::string& name) const
    {
        auto iter = mParameters.find(name);
        if (iter == mParameters.end() || !iter->second.is_boolean())
            return false;
        return iter->second.get<bool>();
    }

    std::string AnimatorComponent::GetCurrentState() const
    {
        return mCurrentStateName;
    }

    int AnimatorComponent::GetClipCount() const
    {
        return static_cast<int>(mClipNames.empty() ? mClips.size() : mClipNames.size());
    }

    std::string AnimatorComponent::GetClipName(int index) const
    {
        if (index < 0)
            return "";

        if (index < static_cast<int>(mClipNames.size()))
            return mClipNames[index];

        if (index < static_cast<int>(mClips.size()))
            return std::to_string(index);

        return "";
    }

    float AnimatorComponent::GetProgress() const
    {
        return mProgress;
    }

    std::string AnimatorComponent::GetControllerPath() const
    {
        return controllerPath;
    }

    void AnimatorComponent::SetControllerPath(const std::string& path)
    {
        const std::string normalizedPath = Utils::NormalizeAssetPath(path);
        if (controllerPath == normalizedPath)
            return;

        controllerPath = normalizedPath;
        bLoaded = false;
    }

    bool AnimatorComponent::EvaluateTransitionCondition(const AnimControllerConditionRes& condition) const
    {
        auto iter = mParameters.find(condition.param);
        if (iter == mParameters.end())
            return false;

        const Json& actual = iter->second;
        const Json& expected = condition.value;

        if (actual.is_boolean() && expected.is_boolean())
        {
            const bool lhs = actual.get<bool>();
            const bool rhs = expected.get<bool>();
            if (condition.op == "==") return lhs == rhs;
            if (condition.op == "!=") return lhs != rhs;
            return false;
        }

        if (actual.is_number() && expected.is_number())
        {
            const double lhs = actual.get<double>();
            const double rhs = expected.get<double>();
            if (condition.op == "==") return lhs == rhs;
            if (condition.op == "!=") return lhs != rhs;
            if (condition.op == ">") return lhs > rhs;
            if (condition.op == ">=") return lhs >= rhs;
            if (condition.op == "<") return lhs < rhs;
            if (condition.op == "<=") return lhs <= rhs;
            return false;
        }

        return false;
    }
}
