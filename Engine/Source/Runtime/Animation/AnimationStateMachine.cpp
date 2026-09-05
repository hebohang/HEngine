#include "Hepch.h"

#include "Runtime/Animation/AnimationStateMachine.h"

#include <algorithm>

namespace HEngine
{
    namespace
    {
        static std::vector<glm::mat4> BlendMatrices(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b, float alpha)
        {
            const size_t count = std::min(a.size(), b.size());
            std::vector<glm::mat4> out(count, glm::mat4(1.0f));
            for (size_t i = 0; i < count; ++i)
            {
                out[i] = a[i] * (1.0f - alpha) + b[i] * alpha;
            }
            return out;
        }
    }

    void AnimationStateMachine::Reset()
    {
        mCurrentNodeId = -1;
        mTargetNodeId = -1;
        mCurrentStateName.clear();
        mInTransition = false;
        mTransitionTime = 0.0f;
        mTransitionDuration = 0.0f;
        mTransitionFromClip = -1;
        mTransitionToClip = -1;
        mMainClipIndex = -1;
        mMainAnimator.Reset();
        mFromAnimator.Reset();
        mToAnimator.Reset();
    }

    void AnimationStateMachine::SetController(const AnimControllerRes& controller)
    {
        mController = &controller;
    }

    void AnimationStateMachine::SetNodeToClipIndex(const std::map<int, int>& nodeToClipIndex)
    {
        mNodeToClipIndex = &nodeToClipIndex;
    }

    void AnimationStateMachine::SetClips(const std::vector<Animation>* clips)
    {
        mClips = clips;
    }

    void AnimationStateMachine::SetParameterStorage(std::map<std::string, Json>* parameters)
    {
        mParameters = parameters;
    }

    bool AnimationStateMachine::IsValid() const
    {
        return mController && mClips && mNodeToClipIndex && !mClips->empty();
    }

    int AnimationStateMachine::GetCurrentNodeId() const
    {
        return mCurrentNodeId;
    }

    std::string AnimationStateMachine::GetCurrentStateName() const
    {
        return mCurrentStateName;
    }

    void AnimationStateMachine::SetCurrentNodeId(int nodeId)
    {
        mCurrentNodeId = nodeId;
        if (mNodeToClipIndex && mNodeToClipIndex->count(nodeId))
        {
            mMainClipIndex = mNodeToClipIndex->at(nodeId);
            if (mClips && mMainClipIndex >= 0 && mMainClipIndex < (int)mClips->size())
            {
                Animation* clip = const_cast<Animation*>(&((*mClips)[mMainClipIndex]));
                mMainAnimator = Animator(clip);
            }
        }
        mCurrentStateName = FindNodeName(nodeId);
    }

    void AnimationStateMachine::Update(float dt, float playSpeed, bool bLoop, std::vector<glm::mat4>& outFinalMatrices, float& outProgress)
    {
        outProgress = 0.0f;
        if (!IsValid())
            return;

        if (mCurrentNodeId < 0)
        {
            SetCurrentNodeId(FindInitialNodeId());
            if (mCurrentNodeId < 0)
                return;
        }

        const float updateDt = dt * std::max(0.01f, playSpeed);

        if (!mInTransition)
        {
            for (const auto& transition : mController->transitions)
            {
                if (transition.fromNodeId != mCurrentNodeId)
                    continue;
                if (!mNodeToClipIndex->count(transition.toNodeId))
                    continue;

                bool allPass = true;
                for (const auto& cond : transition.conditions)
                {
                    if (!EvaluateCondition(cond))
                    {
                        allPass = false;
                        break;
                    }
                }

                if (allPass)
                {
                    mInTransition = true;
                    mTransitionTime = 0.0f;
                    mTransitionDuration = std::max(0.0001f, transition.duration);
                    mTransitionFromClip = mMainClipIndex;
                    mTransitionToClip = mNodeToClipIndex->at(transition.toNodeId);
                    mTargetNodeId = transition.toNodeId;

                    mFromAnimator = mMainAnimator;
                    Animation* toClip = const_cast<Animation*>(&((*mClips)[mTransitionToClip]));
                    mToAnimator = Animator(toClip);
                    break;
                }
            }
        }

        if (mInTransition)
        {
            mTransitionTime += updateDt;
            mFromAnimator.UpdateAnimation(updateDt);
            mToAnimator.UpdateAnimation(updateDt);

            const float alpha = std::clamp(mTransitionTime / mTransitionDuration, 0.0f, 1.0f);
            outFinalMatrices = BlendMatrices(mFromAnimator.GetFinalBoneMatrices(), mToAnimator.GetFinalBoneMatrices(), alpha);
            outProgress = alpha;

            if (alpha >= 1.0f)
            {
                mInTransition = false;
                mCurrentNodeId = mTargetNodeId;
                mMainClipIndex = mTransitionToClip;
                mMainAnimator = mToAnimator;
                mCurrentStateName = FindNodeName(mCurrentNodeId);
            }
        }
        else
        {
            mMainAnimator.UpdateAnimation(updateDt);
            outFinalMatrices = mMainAnimator.GetFinalBoneMatrices();
            outProgress = mMainAnimator.GetProgress();

            if (!bLoop && outProgress >= 0.999f)
                outProgress = 1.0f;
        }

        if (mParameters)
        {
            for (const auto& p : mController->parameters)
            {
                if (p.type == AnimControllerParamType::Trigger)
                    (*mParameters)[p.name] = false;
            }
        }
    }

    bool AnimationStateMachine::EvaluateCondition(const AnimControllerConditionRes& condition) const
    {
        if (!mParameters)
            return false;

        auto iter = mParameters->find(condition.param);
        if (iter == mParameters->end())
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

    std::string AnimationStateMachine::FindNodeName(int nodeId) const
    {
        if (!mController)
            return "";
        for (const auto& node : mController->nodes)
        {
            if (node.id == nodeId)
                return node.name;
        }
        return "";
    }

    int AnimationStateMachine::FindInitialNodeId() const
    {
        if (!mController || !mNodeToClipIndex)
            return -1;

        for (const auto& transition : mController->transitions)
        {
            if (transition.fromNodeId == mController->entryNodeId && mNodeToClipIndex->count(transition.toNodeId))
                return transition.toNodeId;
        }
        if (mNodeToClipIndex->count(mController->entryNodeId))
            return mController->entryNodeId;
        if (!mNodeToClipIndex->empty())
            return mNodeToClipIndex->begin()->first;
        return -1;
    }
}
