#pragma once

#include "Runtime/Animation/Animation.h"
#include "Runtime/Animation/Bone.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace HEngine
{
    struct FiredAnimationEvent
    {
        AnimEventRes Event;
        uint64_t Cycle = 0;
    };

    class Animator
    {
    public:
        Animator() = default;
        Animator(const Animator&) = default;
        explicit Animator(Animation* animation) : mCurrentAnimation(animation) { SamplePose(); }

        void SetLoop(bool value) { mLoop = value; }
        bool HasFinished() const { return !mLoop && mCurrentAnimation && mAbsoluteTime >= mCurrentAnimation->GetDuration(); }

        // Forward fixed stepping emits each crossed event once; zero dt (pause) emits none.
        void UpdateAnimation(float dt)
        {
            if (!std::isfinite(dt) || dt < 0.0f)
                throw std::invalid_argument("Animation step must be finite and nonnegative; seek uses UpdateAnimationAbsolute.");
            if (!mCurrentAnimation || dt == 0.0f) return;
            const double duration = mCurrentAnimation->GetDuration();
            const double next = mAbsoluteTime + mCurrentAnimation->GetTicksPerSecond() * static_cast<double>(dt);
            if (duration <= 0.0) { mCurrentTime = 0.0f; SamplePose(); return; }
            const double end = mLoop ? next : std::min(next, duration);
            if ((end - mAbsoluteTime) / duration > 10000.0)
                throw std::invalid_argument("Animation step exceeds 10000 loops; split the requested step.");
            const uint64_t firstCycle = mLoop ? static_cast<uint64_t>(std::floor(mAbsoluteTime / duration)) : 0;
            const uint64_t lastCycle = mLoop ? static_cast<uint64_t>(std::floor(end / duration)) : 0;
            for (uint64_t cycle = firstCycle; cycle <= lastCycle; ++cycle)
                for (const auto& event : mCurrentAnimation->GetEvents())
                {
                    const double at = cycle * duration + event.timeStamp;
                    if ((at > mAbsoluteTime || (mIncludeStart && at == mAbsoluteTime)) && at <= end)
                        mPendingEvents.push_back({event, cycle});
                }
            mIncludeStart = false;
            mAbsoluteTime = end;
            mCurrentTime = static_cast<float>(mLoop ? std::fmod(end, duration) : end);
            SamplePose();
        }

        // Absolute time is clip ticks. Seeking is pose-only and clears queued events.
        void UpdateAnimationAbsolute(float time)
        {
            if (!std::isfinite(time) || time < 0.0f)
                throw std::invalid_argument("Animation seek time must be finite and nonnegative.");
            mPendingEvents.clear();
            mIncludeStart = false;
            if (!mCurrentAnimation) return;
            const double duration = mCurrentAnimation->GetDuration();
            mAbsoluteTime = duration > 0.0 ? (mLoop ? time : std::min<double>(time, duration)) : 0.0;
            mCurrentTime = duration > 0.0 ? static_cast<float>(mLoop ? std::fmod(mAbsoluteTime, duration) : mAbsoluteTime) : 0.0f;
            SamplePose();
        }

        std::vector<FiredAnimationEvent> ConsumeEvents() { return std::exchange(mPendingEvents, {}); }
        void PlayAnimation(Animation* animation) { mCurrentAnimation = animation; Reset(); }
        // A mesh replacement copies its clip storage. Retarget only that storage;
        // preserve the sampled pose, absolute time, blending and pending events.
        void RebindAnimationStorage(const Animation& previous, Animation& replacement) noexcept
        {
            if (mCurrentAnimation == &previous) mCurrentAnimation = &replacement;
        }
        void SetRootMotionOptions(bool enable, bool lockXZ)
        {
            if (mEnableRootMotion == enable && mLockRootMotionXZ == lockXZ) return;
            mEnableRootMotion = enable; mLockRootMotionXZ = lockXZ; SamplePose();
        }

        void CalculateBoneTransform(const AssimpNodeData* node, glm::mat4 parentTransform)
        {
            if (!node || !mCurrentAnimation) return;
            const LocalPose& bindPose = node->bindPose;
            const Bone* bone = mCurrentAnimation->FindBone(node->name);
            LocalPose pose = bone ? bone->Sample(mCurrentTime, bindPose) : bindPose;
            if (!mEnableRootMotion && node->name.find("Hips_$AssimpFbx$_Translation") != std::string::npos)
            {
                pose.Translation.x = bindPose.Translation.x;
                pose.Translation.z = bindPose.Translation.z;
                if (!mLockRootMotionXZ) pose.Translation.y = bindPose.Translation.y;
            }
            mLocalPose[node->name] = pose;
            const glm::mat4 global = parentTransform * pose.Matrix();
            const auto& bones = mCurrentAnimation->GetBoneIDMap();
            const auto found = bones.find(node->name);
            if (found != bones.end())
            {
                const int index = found->second.id;
                if (index < 0 || index >= 4096) throw std::invalid_argument("Animation bone index is outside [0,4096).");
                if (static_cast<size_t>(index) >= mFinalBoneMatrices.size()) mFinalBoneMatrices.resize(index + 1, glm::mat4(1.0f));
                mFinalBoneMatrices[index] = global * found->second.offset;
            }
            for (const auto& child : node->children)
            {
                mParents[child.name] = node->name;
                CalculateBoneTransform(&child, global);
            }
        }

        // Blend local TRS, then reconstruct the entire hierarchy before inverse bind.
        static std::vector<glm::mat4> BlendFinalMatrices(const Animator& from, const Animator& to, float weight)
        {
            if (!from.mCurrentAnimation || !to.mCurrentAnimation || from.mParents != to.mParents ||
                from.mLocalPose.size() != to.mLocalPose.size())
                throw std::invalid_argument("Animation blend requires matching node hierarchies.");
            const auto& bones = from.mCurrentAnimation->GetBoneIDMap();
            const auto& otherBones = to.mCurrentAnimation->GetBoneIDMap();
            if (bones.size() != otherBones.size()) throw std::invalid_argument("Animation blend requires matching bone palettes.");
            for (const auto& [name, info] : bones)
            {
                const auto other = otherBones.find(name);
                if (other == otherBones.end() || info.id != other->second.id)
                    throw std::invalid_argument("Animation blend has incompatible bone IDs.");
                for (int column = 0; column < 4; ++column)
                    if (glm::length(info.offset[column] - other->second.offset[column]) > 1e-4f)
                        throw std::invalid_argument("Animation blend has incompatible inverse bind poses.");
            }
            std::vector<glm::mat4> result(std::max(from.mFinalBoneMatrices.size(), to.mFinalBoneMatrices.size()), glm::mat4(1.0f));
            const auto visit = [&](const auto& self, const AssimpNodeData& node, const glm::mat4& parent) -> void
            {
                const auto a = from.mLocalPose.find(node.name), b = to.mLocalPose.find(node.name);
                if (a == from.mLocalPose.end() || b == to.mLocalPose.end())
                    throw std::invalid_argument("Animation blend is missing a node pose.");
                const glm::mat4 global = parent * LocalPose::Blend(a->second, b->second, weight).Matrix();
                const auto bone = bones.find(node.name);
                if (bone != bones.end()) result.at(bone->second.id) = global * bone->second.offset;
                for (const auto& child : node.children) self(self, child, global);
            };
            visit(visit, from.mCurrentAnimation->GetRootNode(), glm::mat4(1.0f));
            return result;
        }

        void OverrideBlendedPose(const Animator& from, const Animator& to, float weight)
        {
            const auto palette = BlendFinalMatrices(from, to, weight);
            auto local = from.mLocalPose;
            for (auto& [name, pose] : local) pose = LocalPose::Blend(pose, to.mLocalPose.at(name), weight);
            mLocalPose = std::move(local);
            mParents = from.mParents;
            mFinalBoneMatrices = palette;
        }

        void OverrideWeightedPose(const std::vector<Animator>& sources, const std::vector<float>& weights)
        {
            if (sources.empty() || sources.size() != weights.size() || sources.size() > 32)
                throw std::invalid_argument("Weighted animation requires 1..32 matching sources and weights.");
            double total = 0;
            size_t anchor = 0;
            for (size_t i = 0; i < sources.size(); ++i)
            {
                if (!std::isfinite(weights[i]) || weights[i] < 0) throw std::invalid_argument("Animation weights must be finite and nonnegative.");
                total += weights[i];
                if (weights[i] > weights[anchor]) anchor = i;
                (void)BlendFinalMatrices(sources.front(), sources[i], 0); // Validate hierarchy, bone IDs and inverse binds.
            }
            if (total <= 0) throw std::invalid_argument("Animation weights have zero sum.");
            Animator candidate = sources.front();
            for (auto& [name, pose] : candidate.mLocalPose)
            {
                glm::vec3 translation(0), scale(0);
                glm::quat rotation(0, 0, 0, 0);
                const auto reference = sources[anchor].mLocalPose.at(name).Rotation;
                for (size_t i = 0; i < sources.size(); ++i)
                {
                    const auto& sample = sources[i].mLocalPose.at(name);
                    const float weight = static_cast<float>(weights[i] / total);
                    translation += sample.Translation * weight;
                    scale += sample.Scale * weight;
                    rotation += (glm::dot(reference, sample.Rotation) < 0 ? -sample.Rotation : sample.Rotation) * weight;
                }
                if (glm::length(rotation) < 1e-6f) throw std::invalid_argument("Weighted rotation is degenerate.");
                pose = {translation, glm::normalize(rotation), scale};
            }
            const auto palette = BlendFinalMatrices(candidate, candidate, 0);
            mLocalPose = std::move(candidate.mLocalPose); mParents = candidate.mParents; mFinalBoneMatrices = palette;
        }

        void ApplyLayer(const Animator& layer, float weight, const std::vector<std::string>& maskRoots, bool additive)
        {
            if (!std::isfinite(weight) || weight < 0 || weight > 1)
                throw std::invalid_argument("Animation layer weight must be in [0,1].");
            (void)BlendFinalMatrices(*this, layer, 0);
            for (const auto& root : maskRoots)
                if (!mLocalPose.contains(root)) throw std::invalid_argument("Animation mask names an unknown hierarchy node: " + root);
            auto candidate = *this;
            const auto apply = [&](const auto& self, const AssimpNodeData& node, bool included) -> void {
                included |= maskRoots.empty() || std::find(maskRoots.begin(), maskRoots.end(), node.name) != maskRoots.end();
                if (included)
                {
                    auto& pose = candidate.mLocalPose.at(node.name);
                    const auto& source = layer.mLocalPose.at(node.name);
                    if (!additive) pose = LocalPose::Blend(pose, source, weight);
                    else
                    {
                        const auto& bind = node.bindPose;
                        if (std::abs(bind.Scale.x) < 1e-6f || std::abs(bind.Scale.y) < 1e-6f || std::abs(bind.Scale.z) < 1e-6f)
                            throw std::invalid_argument("Additive animation requires nonsingular bind scale.");
                        pose.Translation += (source.Translation - bind.Translation) * weight;
                        pose.Rotation = glm::normalize(pose.Rotation * glm::slerp(glm::quat(1,0,0,0), glm::inverse(bind.Rotation) * source.Rotation, weight));
                        pose.Scale *= glm::mix(glm::vec3(1), source.Scale / bind.Scale, weight);
                    }
                }
                for (const auto& child : node.children) self(self, child, included);
            };
            apply(apply, layer.mCurrentAnimation->GetRootNode(), false);
            const auto palette = BlendFinalMatrices(candidate, candidate, 0);
            mLocalPose = std::move(candidate.mLocalPose); mFinalBoneMatrices = palette;
        }

        const std::vector<glm::mat4>& GetFinalBoneMatrices() const { return mFinalBoneMatrices; }
        bool HasCurrentAnimation() const { return mCurrentAnimation != nullptr; }
        // Restore the clip pose without seeking or clearing this step's events.
        void ResamplePose() { SamplePose(); }
        float GetProgress() const
        {
            return mCurrentAnimation && mCurrentAnimation->GetDuration() > 0.0f ?
                mCurrentTime / mCurrentAnimation->GetDuration() : 0.0f;
        }
        float GetCurrentTime() const { return mCurrentTime; }
        void Reset() { mCurrentTime = 0.0f; mAbsoluteTime = 0.0; mIncludeStart = true; mPendingEvents.clear(); SamplePose(); }
        void ClearAnimation() { mCurrentAnimation = nullptr; Reset(); }

    private:
        void SamplePose()
        {
            mLocalPose.clear(); mParents.clear();
            mFinalBoneMatrices.clear();
            if (!mCurrentAnimation) return;
            const auto& root = mCurrentAnimation->GetRootNode();
            if (root.name.empty() && root.children.empty()) return;
            mParents[root.name] = {};
            CalculateBoneTransform(&root, glm::mat4(1.0f));
        }
        std::vector<glm::mat4> mFinalBoneMatrices;
        std::map<std::string, LocalPose> mLocalPose;
        std::map<std::string, std::string> mParents;
        std::vector<FiredAnimationEvent> mPendingEvents;
        Animation* mCurrentAnimation = nullptr;
        float mCurrentTime = 0.0f;
        double mAbsoluteTime = 0.0;
        bool mLoop = true;
        bool mIncludeStart = true;
        bool mEnableRootMotion = false;
        bool mLockRootMotionXZ = true;
    };
}
