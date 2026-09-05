#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/Animation/Animation.h"
#include "Runtime/Animation/Animator.h"
#include "Runtime/Resource/ResType/AnimController.h"

#include <string>
#include <vector>
#include <map>

namespace HEngine
{
    REFLECTION_TYPE(AnimatorComponent)
    CLASS(AnimatorComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(AnimatorComponent);
    public:
        AnimatorComponent() = default;
        AnimatorComponent(const AnimatorComponent&) = default;
        explicit AnimatorComponent(const std::string& path)
            : animResPath(path)
        {
        }

        void Play(int clipIndex = -1);
        void Stop();
        void SetSpeed(float speed);

        void SetBool(const std::string& name, bool value);
        void SetInt(const std::string& name, int value);
        void SetFloat(const std::string& name, float value);
        void SetTrigger(const std::string& name);
        void ResetTrigger(const std::string& name);

        [[nodiscard]] bool HasParameter(const std::string& name) const;
        [[nodiscard]] bool GetBool(const std::string& name, bool defaultValue = false) const;
        [[nodiscard]] int GetInt(const std::string& name, int defaultValue = 0) const;
        [[nodiscard]] float GetFloat(const std::string& name, float defaultValue = 0.0f) const;
        [[nodiscard]] bool GetTrigger(const std::string& name) const;

        [[nodiscard]] std::string GetCurrentState() const;

        [[nodiscard]] int GetClipCount() const;
        [[nodiscard]] std::string GetClipName(int index) const;
        [[nodiscard]] float GetProgress() const;

        [[nodiscard]] std::string GetControllerPath() const;
        void SetControllerPath(const std::string& path);

        bool EvaluateTransitionCondition(const AnimControllerConditionRes& condition) const;

    public:
        std::string controllerPath = "None";
        std::string animResPath = "None";
        int         currentClipIndex = 0;
        bool        bPlay = true;
        bool        bLoop = true;
        float       playSpeed = 1.0f;
        bool        bEnableRootMotion = false;
        bool        bLockRootMotionXZ = true;

        PROPERTY(Disable)
        bool bLoaded = false;

        PROPERTY(Disable)
        int mLastClipIndex = -1;

        PROPERTY(Disable)
        int mBoneCount = 0;

        PROPERTY(Disable)
        float mProgress = 0.0f;

        PROPERTY(Disable)
        std::vector<Animation> mClips;

        PROPERTY(Disable)
        std::vector<std::string> mClipNames;

        PROPERTY(Disable)
        std::map<std::string, BoneInfo> mBoneInfoMap;

        PROPERTY(Disable)
        AnimControllerRes mControllerRes;

        PROPERTY(Disable)
        std::map<std::string, Json> mParameters;

        PROPERTY(Disable)
        std::map<std::string, AnimControllerParamType> mParameterTypes;

        PROPERTY(Disable)
        std::map<int, int> mNodeToClipIndex;

        PROPERTY(Disable)
        int mCurrentNodeId = -1;

        PROPERTY(Disable)
        int mTargetNodeId = -1;

        PROPERTY(Disable)
        std::string mCurrentStateName;

        PROPERTY(Disable)
        bool mInTransition = false;

        PROPERTY(Disable)
        float mTransitionTime = 0.0f;

        PROPERTY(Disable)
        float mTransitionDuration = 0.0f;

        PROPERTY(Disable)
        int mTransitionFromClipIndex = -1;

        PROPERTY(Disable)
        int mTransitionToClipIndex = -1;

        PROPERTY(Disable)
        Animator mTransitionFromAnimator;

        PROPERTY(Disable)
        Animator mTransitionToAnimator;
    };
    REGISTER_COMPONENT_TYPE(AnimatorComponent)
}
