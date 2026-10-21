#pragma once

#include "Runtime/Animation/Animation.h"
#include "Runtime/Animation/Animator.h"
#include "Runtime/Resource/ResType/AnimController.h"

#include <map>
#include <string>

namespace HEngine
{
    class AnimationStateMachine
    {
    public:
        void Reset();
        std::vector<FiredAnimationEvent> ConsumeEvents()
        {
            if (mInTransition) { (void)mFromAnimator.ConsumeEvents(); return mToAnimator.ConsumeEvents(); }
            return mMainAnimator.ConsumeEvents();
        }

        void SetController(const AnimControllerRes& controller);
        void SetNodeToClipIndex(const std::map<int, int>& nodeToClipIndex);
        void SetClips(const std::vector<Animation>* clips);
        void SetParameterStorage(std::map<std::string, Json>* parameters);

        bool IsValid() const;
        int GetCurrentNodeId() const;
        std::string GetCurrentStateName() const;

        void SetCurrentNodeId(int nodeId);

        void Update(float dt, float playSpeed, bool bLoop, std::vector<glm::mat4>& outFinalMatrices, float& outProgress);

    private:
        bool EvaluateCondition(const AnimControllerConditionRes& condition) const;
        std::string FindNodeName(int nodeId) const;
        int FindInitialNodeId() const;

        const AnimControllerRes* mController = nullptr;
        const std::vector<Animation>* mClips = nullptr;
        const std::map<int, int>* mNodeToClipIndex = nullptr;
        std::map<std::string, Json>* mParameters = nullptr;

        int mCurrentNodeId = -1;
        int mTargetNodeId = -1;
        std::string mCurrentStateName;

        bool mInTransition = false;
        float mTransitionTime = 0.0f;
        float mTransitionDuration = 0.0f;
        int mTransitionFromClip = -1;
        int mTransitionToClip = -1;

        Animator mMainAnimator;
        Animator mFromAnimator;
        Animator mToAnimator;
        int mMainClipIndex = -1;
    };
}
