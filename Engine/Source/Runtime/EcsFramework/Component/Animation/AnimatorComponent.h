#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include "Runtime/Animation/Animation.h"
#include "Runtime/Animation/Animator.h"
#include "Runtime/Animation/SecondaryMotion.h"
#include "Runtime/Animation/ClothSimulation.h"
#include "Runtime/Animation/CharacterPose.h"
#include "Runtime/Animation/FacialPerformance.h"
#include "Runtime/Resource/ResType/AnimController.h"

#include <string>
#include <vector>
#include <map>

namespace HEngine
{
    class Mesh;
    REFLECTION_TYPE(AnimatorComponent)
    CLASS(AnimatorComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(AnimatorComponent);
    public:
        AnimatorComponent() = default;
        AnimatorComponent(const AnimatorComponent&) = default;
        AnimatorComponent& operator=(const AnimatorComponent&) = default;
        AnimatorComponent(AnimatorComponent&&) noexcept = default;
        AnimatorComponent& operator=(AnimatorComponent&&) noexcept = default;
        explicit AnimatorComponent(const std::string& path)
            : animResPath(path)
        {
        }

        void Play(int clipIndex = -1);
        void Stop();
        void Pause();
        void Resume();
        void Step(float seconds);
        Json ConsumeEvents();
        void SetSpeed(float speed);
        void RebindCompatibleMesh(Mesh& previous, Mesh& replacement);

        void SetBool(const std::string& name, bool value);
        void SetInt(const std::string& name, int value);
        void SetFloat(const std::string& name, float value);
        void SetTrigger(const std::string& name);
        void ResetTrigger(const std::string& name);

        [[nodiscard]] bool HasParameter(const std::string& name) const;
        [[nodiscard]] bool IsParameterType(const std::string& name, AnimControllerParamType type) const;
        [[nodiscard]] bool GetBool(const std::string& name, bool defaultValue = false) const;
        [[nodiscard]] int GetInt(const std::string& name, int defaultValue = 0) const;
        [[nodiscard]] float GetFloat(const std::string& name, float defaultValue = 0.0f) const;
        [[nodiscard]] bool GetTrigger(const std::string& name) const;

        [[nodiscard]] std::string GetCurrentState() const;

        [[nodiscard]] int GetClipCount() const;
        [[nodiscard]] int GetCurrentClipIndex() const;
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

        std::vector<SecondaryMotionBone> SecondaryBones;
        std::vector<SecondaryMotionSphere> SecondaryColliders;
        glm::vec3 SecondaryWind = glm::vec3(0.0f);
        // 0 inherits DeformationDistance for existing scenes. A positive
        // distance keeps inexpensive bone motion visible after cloth/morph LOD.
        float SecondaryMotionDistance = 0.0f;

        std::vector<ClothPanel> ClothPanels;
        std::vector<ClothCapsule> ClothColliders;
        float DeformationDistance = 25.0f;
        std::vector<FootContact> Feet;
        std::string PelvisBone;
        std::vector<BoneLookAt> LookAt;
        std::vector<LimbConstraint> LimbTargets;
        std::map<std::string, float> FacialWeights;
        std::string FacialPerformancePath;
        FacialPerformanceRes FacialPerformanceData;
        std::string FacialBlinkLeft;
        std::string FacialBlinkRight;
        float FacialBlinkInterval = 0.0f;

        PROPERTY(Disable)
        FacialPlayer mFacialPlayer;
        PROPERTY(Disable)
        CharacterPose mCharacterPose;
        PROPERTY(Disable)
        std::map<int, ClothRenderState> mClothSimulation;
        PROPERTY(Disable)
        bool mDeformationWasActive = false;
        PROPERTY(Disable)
        glm::vec3 mDeformationOrigin = glm::vec3(0);

        PROPERTY(Disable)
        SecondaryMotion mSecondaryMotion;

        PROPERTY(Disable)
        bool bPaused = false;

        PROPERTY(Disable)
        bool bHoldFinalPose = false;

        PROPERTY(Disable)
        float mStepSeconds = 0.0f;

        PROPERTY(Disable)
        Json mAnimationEvents = Json::array();

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
