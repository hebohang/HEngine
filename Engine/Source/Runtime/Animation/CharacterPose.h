#pragma once

#include "Runtime/Animation/Animation.h"
#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace HEngine
{
    REFLECTION_TYPE(FootContact)
    STRUCT(FootContact, Fields)
    {
        REFLECTION_BODY(FootContact);
        std::string Upper;
        std::string Lower;
        std::string Foot;
        float SoleHeight = 0.07f;
        float Weight = 1.0f;
        float PlantHeight = 0.12f;
        float MaxCorrection = 0.35f;
        bool LockFoot = true;
    };

    REFLECTION_TYPE(BoneLookAt)
    STRUCT(BoneLookAt, Fields)
    {
        REFLECTION_BODY(BoneLookAt);
        std::string Bone;
        glm::vec3 LocalForward = glm::vec3(0, 0, 1);
        glm::vec3 WorldTarget = glm::vec3(0, 1.6f, 3);
        float Weight = 0.0f;
        float MaxAngle = 40.0f;
    };

    REFLECTION_TYPE(LimbConstraint)
    STRUCT(LimbConstraint, Fields)
    {
        REFLECTION_BODY(LimbConstraint);
        std::string Upper;
        std::string Lower;
        std::string End;
        // Contact point in the End bone's local coordinates (e.g. a fingertip
        // beyond the final weighted joint). Zero preserves joint-origin IK.
        glm::vec3 EffectorLocalOffset = glm::vec3(0);
        glm::vec3 WorldRotationDegrees = glm::vec3(0);
        float RotationWeight = 0.0f;
        std::string TargetUUID;
        // Optional target in this rig's current evaluated bone frame (e.g. support hand on a held prop).
        std::string TargetBone;
        std::string WeightParameter;
        glm::vec3 TargetOffset = glm::vec3(0);
        glm::vec3 WorldTarget = glm::vec3(0);
        float Weight = 0.0f;
    };

    class CharacterPose
    {
    public:
        using FloorQuery = std::function<bool(const glm::vec3&, float&)>;
        void Reset() { mFeet.clear(); mPelvis = 0; }

        struct FootContactDiagnostic
        {
            glm::vec3 Animated{0}, Anchor{0}, Target{0};
            float Weight = 0;
            bool Locked = false, Valid = false;
        };

        // Snapshot of current CPU solver state, not the submitted render palette.
        // Invalid contacts may retain coordinates from the last valid evaluation.
        [[nodiscard]] std::vector<FootContactDiagnostic> GetFootContactDiagnostics(size_t limit = 16) const
        {
            std::vector<FootContactDiagnostic> result;
            const size_t count = std::min(limit, mFeet.size());
            result.reserve(count);
            for (size_t index = 0; index < count; ++index)
            {
                const auto& state = mFeet[index];
                result.push_back({state.Animated, state.Anchor, state.Target, state.Weight,
                    state.Locked, state.Valid});
            }
            return result;
        }

        static bool BoneMatrix(const Animation& animation, const std::vector<glm::mat4>& palette,
            const std::string& name, glm::mat4& matrix)
        {
            const auto found = animation.GetBoneIDMap().find(name);
            if (found == animation.GetBoneIDMap().end() || found->second.id < 0 ||
                static_cast<size_t>(found->second.id) >= palette.size()) return false;
            const float determinant = glm::determinant(found->second.offset);
            if (!Finite(found->second.offset) || !Finite(palette[found->second.id]) ||
                !std::isfinite(determinant) || std::abs(determinant) < 1e-10f) return false;
            matrix = palette[found->second.id] * glm::inverse(found->second.offset);
            return Finite(matrix);
        }

        static void TransformSubtree(const Animation& animation, const std::string& name,
            const glm::mat4& delta, std::vector<glm::mat4>& palette)
        {
            if (!Finite(delta)) return;
            const auto visit = [&](const auto& self, const AssimpNodeData& node, bool affected) -> void
            {
                affected = affected || node.name == name;
                const auto found = animation.GetBoneIDMap().find(node.name);
                if (affected && found != animation.GetBoneIDMap().end() && found->second.id >= 0 &&
                    static_cast<size_t>(found->second.id) < palette.size()) palette[found->second.id] = delta * palette[found->second.id];
                for (const auto& child : node.children) self(self, child, affected);
            };
            visit(visit, animation.GetRootNode(), false);
        }

        // Analytic two-link IK, preserving the animated knee bend plane.
        static bool SolveTwoBone(const Animation& animation, const std::string& upper,
            const std::string& lower, const std::string& end, const glm::vec3& target,
            std::vector<glm::mat4>& palette, const glm::vec3& effectorLocalOffset = glm::vec3(0))
        {
            glm::mat4 a, b, c;
            if (!BoneMatrix(animation, palette, upper, a) || !BoneMatrix(animation, palette, lower, b) ||
                !BoneMatrix(animation, palette, end, c) || !Finite(target) || !Finite(effectorLocalOffset)) return false;
            const glm::vec3 root(a[3]), knee(b[3]), foot(c * glm::vec4(effectorLocalOffset, 1));
            const float first = glm::distance(root, knee), second = glm::distance(knee, foot);
            const glm::vec3 delta = target - root;
            const float targetDistance = glm::length(delta);
            if (!std::isfinite(first) || !std::isfinite(second) || !std::isfinite(targetDistance) ||
                first < 1e-5f || second < 1e-5f || targetDistance < 1e-5f) return false;
            const glm::vec3 axis = glm::normalize(delta);
            const float margin = std::min(1e-4f, std::min(first, second) * .01f);
            const float distance = std::clamp(targetDistance, std::abs(first - second) + margin, first + second - margin);
            glm::vec3 bend = knee - root - axis * glm::dot(knee - root, axis);
            if (glm::length(bend) < 1e-5f) bend = glm::cross(axis, std::abs(axis.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
            bend = glm::normalize(bend);
            const float along = (first * first + distance * distance - second * second) / (2 * distance);
            const glm::vec3 newKnee = root + axis * along + bend * std::sqrt(std::max(0.0f, first * first - along * along));
            Rotate(animation, upper, root, knee - root, newKnee - root, palette);
            BoneMatrix(animation, palette, lower, b);
            BoneMatrix(animation, palette, end, c);
            Rotate(animation, lower, glm::vec3(b[3]), glm::vec3(c * glm::vec4(effectorLocalOffset, 1)) - glm::vec3(b[3]),
                root + axis * distance - glm::vec3(b[3]), palette);
            return true;
        }

        // World-space rigid rotations preserve segment lengths even for a scaled actor.
        // Keep the end's stretch/shear matrix while replacing its orthonormal orientation.
        static bool SolveLimbPose(const Animation& animation, const LimbConstraint& limb,
            const glm::mat4& world, const glm::vec3& worldTarget, std::vector<glm::mat4>& palette)
        {
            if (!Finite(world) || !Finite(worldTarget) || !Finite(limb.EffectorLocalOffset) ||
                !std::isfinite(limb.Weight) || !std::isfinite(limb.RotationWeight) ||
                !Finite(limb.WorldRotationDegrees) || !std::isfinite(glm::determinant(world)) ||
                std::abs(glm::determinant(world)) < 1e-8f) return false;
            const float weight = std::clamp(limb.Weight, 0.0f, 1.0f);
            if (weight <= 0) return true;
            glm::mat4 original;
            if (!BoneMatrix(animation, palette, limb.End, original)) return false;
            const glm::mat4 inverseWorld = glm::inverse(world);
            if (limb.RotationWeight <= 0)
                return SolveTwoBone(animation, limb.Upper, limb.Lower, limb.End,
                    glm::mix(glm::vec3(original * glm::vec4(limb.EffectorLocalOffset, 1)),
                        glm::vec3(inverseWorld * glm::vec4(worldTarget, 1)), weight), palette, limb.EffectorLocalOffset);
            const glm::mat4 endWorld = world * original;
            glm::vec3 x(endWorld[0]), y(endWorld[1]);
            if (glm::length(x) < 1e-6f) return false;
            x = glm::normalize(x);
            y -= x * glm::dot(x, y);
            if (glm::length(y) < 1e-6f) return false;
            y = glm::normalize(y);
            const glm::mat3 orientation(x, y, glm::cross(x, y));
            const glm::mat3 stretch = glm::transpose(orientation) * glm::mat3(endWorld);
            const glm::quat desired = glm::quat(glm::radians(limb.WorldRotationDegrees));
            const glm::mat3 finalLinear = glm::mat3_cast(glm::slerp(glm::quat_cast(orientation), desired,
                weight * std::clamp(limb.RotationWeight, 0.0f, 1.0f))) * stretch;
            const glm::vec3 contact = glm::mix(glm::vec3(endWorld * glm::vec4(limb.EffectorLocalOffset, 1)), worldTarget, weight);
            const glm::vec3 originTarget = contact - finalLinear * limb.EffectorLocalOffset;
            auto candidate = palette;
            for (auto& matrix : candidate) matrix = world * matrix;
            if (!SolveTwoBone(animation, limb.Upper, limb.Lower, limb.End, originTarget, candidate)) return false;
            glm::mat4 solved;
            if (!BoneMatrix(animation, candidate, limb.End, solved) || std::abs(glm::determinant(solved)) < 1e-8f) return false;
            glm::mat4 finalEnd(finalLinear);
            // Unreachable targets retain the clamped IK endpoint rather than stretching the limb.
            finalEnd[3] = solved[3];
            TransformSubtree(animation, limb.End, finalEnd * glm::inverse(solved), candidate);
            for (auto& matrix : candidate)
            {
                matrix = inverseWorld * matrix;
                if (!Finite(matrix)) return false;
            }
            palette.swap(candidate);
            return true;
        }

        void ApplyFeet(float dt, const glm::mat4& world, const Animation& animation,
            const std::vector<FootContact>& settings, const std::string& pelvis,
            const FloorQuery& query, bool grounded, std::vector<glm::mat4>& palette)
        {
            if (!std::isfinite(dt) || dt <= 0 || !Finite(world) ||
                !std::isfinite(glm::determinant(world)) || std::abs(glm::determinant(world)) < 1e-8f) return;
            const glm::vec3 origin(world[3]);
            if (dt > .25f || glm::distance(origin, mOrigin) > 2) Reset();
            const float actorSpeed = glm::length(glm::vec2(origin.x - mOrigin.x, origin.z - mOrigin.z)) / dt;
            mOrigin = origin;
            mFeet.resize(settings.size());
            const glm::mat4 inverseWorld = glm::inverse(world);
            float pelvisTarget = 0;
            bool hasEligibleFoot = false;
            for (size_t i = 0; i < settings.size(); ++i)
            {
                auto& state = mFeet[i];
                const auto& foot = settings[i];
                if (!std::isfinite(foot.Weight) || foot.Weight <= 0 ||
                    !std::isfinite(foot.SoleHeight) || !std::isfinite(foot.PlantHeight) || foot.PlantHeight < 0 ||
                    !std::isfinite(foot.MaxCorrection) || foot.MaxCorrection < 0)
                { state = {}; continue; }
                const float weight = std::clamp(foot.Weight, 0.0f, 1.0f);
                glm::mat4 matrix;
                if (!BoneMatrix(animation, palette, foot.Foot, matrix)) { state = {}; continue; }
                const glm::vec3 animated(world * matrix * glm::vec4(0, 0, 0, 1));
                float height = 0;
                state.Valid = Finite(animated) && grounded && query(animated, height) && std::isfinite(height);
                if (!state.Valid) { state = {}; continue; }
                hasEligibleFoot = true;
                const float correction = std::clamp(height + foot.SoleHeight - animated.y, -foot.MaxCorrection, foot.MaxCorrection);
                const bool planted = animated.y - height - foot.SoleHeight <= foot.PlantHeight;
                // A successful floor ray does not imply contact: preserve the
                // animated swing instead of pulling every airborne foot down
                // and lowering the pelvis throughout the stride.
                if (!planted) { state = {}; continue; }
                const float footSpeed = glm::length(glm::vec2(animated.x - state.Animated.x,
                    animated.z - state.Animated.z)) / dt;
                // A stance lock must survive a moving actor's stride, including
                // brief overreach. Once both the actor and the authored foot have
                // settled, ease a leftover lock toward the new idle contact.
                // This also handles a clip transition that never lifts the foot.
                state.StillSeconds = actorSpeed < .02f && footSpeed < .12f
                    ? state.StillSeconds + dt : 0.0f;
                if (!foot.LockFoot) state.Locked = false;
                if (!state.Locked)
                {
                    state.Anchor = animated + glm::vec3(0, correction, 0);
                    state.Locked = planted && foot.LockFoot;
                }
                else if (state.StillSeconds > .2f)
                {
                    state.Anchor = glm::mix(state.Anchor, animated + glm::vec3(0, correction, 0),
                        1.0f - std::exp(-10.0f * dt));
                }
                // Fade contact at lift-off rather than snapping by PlantHeight
                // on the first swing frame. Keep the locked anchor unblended:
                // overwriting it with each weighted target causes cumulative slip.
                const float separation = std::max(0.0f, animated.y - height - foot.SoleHeight);
                const float contact = foot.PlantHeight > 1e-6f
                    ? 1.0f - glm::smoothstep(0.0f, foot.PlantHeight, separation) : 1.0f;
                state.Weight = weight * contact;
                // Keep the stance anchor until swing. SolveTwoBone already
                // clamps an unreachable endpoint continuously to the true
                // bone lengths; blending back to a distant animated foot here
                // creates a second, discontinuous target at the reach limit.
                state.Target = glm::mix(animated, state.Anchor, state.Weight);
                // Always query at the resulting XZ position, including release.
                // Switching between anchor/animated floor heights at unlock
                // would create a second discontinuity on slopes and stairs.
                float targetHeight = 0;
                if (!query(state.Target, targetHeight) || !std::isfinite(targetHeight))
                { state = {}; continue; }
                const float targetCorrection = std::clamp(targetHeight + foot.SoleHeight - animated.y,
                    -foot.MaxCorrection, foot.MaxCorrection);
                state.Target.y = animated.y + targetCorrection * state.Weight;
                state.Animated = animated;
                pelvisTarget = std::min(pelvisTarget, targetCorrection * state.Weight);
            }
            // Preserve decay through the last foot's lift-off. Disabled/invalid
            // configurations still clear history immediately.
            if (!hasEligibleFoot) mPelvis = 0;
            else mPelvis += (std::max(-.25f, pelvisTarget) - mPelvis) * (1 - std::exp(-12 * dt));
            if (!pelvis.empty())
                TransformSubtree(animation, pelvis, inverseWorld * glm::translate(glm::mat4(1), glm::vec3(0, mPelvis, 0)) * world, palette);
            for (size_t i = 0; i < settings.size(); ++i)
            {
                const auto& state = mFeet[i];
                const auto& foot = settings[i];
                if (!state.Valid) continue;
                glm::mat4 animatedFoot, solvedFoot;
                if (!BoneMatrix(animation, palette, foot.Foot, animatedFoot) ||
                    !SolveTwoBone(animation, foot.Upper, foot.Lower, foot.Foot,
                        glm::vec3(inverseWorld * glm::vec4(state.Target, 1)), palette) ||
                    !BoneMatrix(animation, palette, foot.Foot, solvedFoot)) continue;
                // Position IK rotates the lower-leg subtree, including the
                // foot. Preserve the authored sole orientation before adding
                // the terrain tilt; otherwise ankle correction itself can
                // drive a previously level forefoot through a flat floor.
                animatedFoot[3] = solvedFoot[3];
                TransformSubtree(animation, foot.Foot, animatedFoot * glm::inverse(solvedFoot), palette);
                // Sample the same floor around the sole to obtain its local slope.
                float hx, hz, center;
                if (query(state.Target, center) && query(state.Target + glm::vec3(.08f, 0, 0), hx) &&
                    query(state.Target + glm::vec3(0, 0, .08f), hz) &&
                    std::isfinite(center) && std::isfinite(hx) && std::isfinite(hz))
                {
                    glm::mat4 matrix;
                    if (!BoneMatrix(animation, palette, foot.Foot, matrix)) continue;
                    const glm::vec3 normal = glm::normalize(glm::vec3(-(hx - center) / .08f, 1, -(hz - center) / .08f));
                    if (normal.y > .65f)
                    {
                        const glm::vec3 localNormal = glm::normalize(glm::mat3(inverseWorld) * normal);
                        const glm::vec3 localUp = glm::normalize(glm::mat3(inverseWorld) * glm::vec3(0, 1, 0));
                        Rotate(animation, foot.Foot, glm::vec3(matrix[3]), localUp,
                            glm::mix(localUp, localNormal, state.Weight), palette);
                    }
                }
            }
        }

        static void ApplyLook(const glm::mat4& world, const Animation& animation,
            const std::vector<BoneLookAt>& settings, std::vector<glm::mat4>& palette)
        {
            if (!Finite(world) || !std::isfinite(glm::determinant(world)) ||
                std::abs(glm::determinant(world)) < 1e-8f) return;
            const glm::mat4 inverseWorld = glm::inverse(world);
            for (const auto& look : settings)
            {
                glm::mat4 matrix;
                if (!std::isfinite(look.Weight) || look.Weight <= 0 || !std::isfinite(look.MaxAngle) ||
                    !Finite(look.WorldTarget) || !Finite(look.LocalForward) ||
                    !BoneMatrix(animation, palette, look.Bone, matrix)) continue;
                const glm::vec3 forward = glm::mat3(matrix) * look.LocalForward;
                const glm::vec3 direction = glm::vec3(inverseWorld * glm::vec4(look.WorldTarget, 1)) - glm::vec3(matrix[3]);
                if (glm::length(forward) < 1e-6f || glm::length(direction) < 1e-6f) continue;
                const float angle = std::acos(std::clamp(glm::dot(glm::normalize(forward), glm::normalize(direction)), -1.0f, 1.0f));
                const float amount = angle > 1e-5f ? std::min(1.0f, glm::radians(std::clamp(look.MaxAngle, 0.0f, 85.0f)) / angle) : 1;
                const glm::quat rotation = glm::slerp(glm::quat(1, 0, 0, 0), glm::rotation(glm::normalize(forward), glm::normalize(direction)), amount * std::clamp(look.Weight, 0.0f, 1.0f));
                TransformSubtree(animation, look.Bone, glm::translate(glm::mat4(1), glm::vec3(matrix[3])) *
                    glm::toMat4(rotation) * glm::translate(glm::mat4(1), -glm::vec3(matrix[3])), palette);
            }
        }

    private:
        static bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        static bool Finite(const glm::mat4& matrix)
        {
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    if (!std::isfinite(matrix[column][row])) return false;
            return true;
        }
        static void Rotate(const Animation& animation, const std::string& bone, const glm::vec3& pivot,
            const glm::vec3& from, const glm::vec3& to, std::vector<glm::mat4>& palette)
        {
            if (!Finite(pivot) || !Finite(from) || !Finite(to) ||
                !std::isfinite(glm::length(from)) || !std::isfinite(glm::length(to)) ||
                glm::length(from) < 1e-6f || glm::length(to) < 1e-6f) return;
            TransformSubtree(animation, bone, glm::translate(glm::mat4(1), pivot) *
                glm::toMat4(glm::rotation(glm::normalize(from), glm::normalize(to))) * glm::translate(glm::mat4(1), -pivot), palette);
        }
        struct FootState
        {
            glm::vec3 Anchor{0}, Target{0}, Animated{0};
            float Weight = 0;
            float StillSeconds = 0;
            bool Locked = false, Valid = false;
        };
        std::vector<FootState> mFeet;
        glm::vec3 mOrigin{0};
        float mPelvis = 0;
    };
}
