#pragma once

#include "Runtime/Animation/Animation.h"
#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace HEngine
{
    // Tail is in bone-local bind units. Declare parents before their children.
    REFLECTION_TYPE(SecondaryMotionBone)
    STRUCT(SecondaryMotionBone, Fields)
    {
        REFLECTION_BODY(SecondaryMotionBone);
        std::string Bone;
        glm::vec3 Tail = glm::vec3(0.0f, 0.15f, 0.0f);
        float Stiffness = 90.0f;
        float Damping = 12.0f;
        float GravityScale = 0.1f;
        // Acceleration opposing root speed (1/s), for authored movement airflow.
        float MotionDrag = 0.0f;
        float MaxAngleDegrees = 30.0f;
        float Radius = 0.015f;
    };

    REFLECTION_TYPE(SecondaryMotionSphere)
    STRUCT(SecondaryMotionSphere, Fields)
    {
        REFLECTION_BODY(SecondaryMotionSphere);
        std::string Bone;
        glm::vec3 Offset = glm::vec3(0.0f);
        float Radius = 0.1f;
    };

    // Per-instance, world-space spring tips; final skin matrices are the only output.
    // Never changes animation clips, inverse binds, or shared resource data.
    class SecondaryMotion
    {
    public:
        void Reset() { mStates.clear(); }
        bool HasState() const { return !mStates.empty(); }

        void Apply(float dt, const glm::mat4& world, const Animation& animation,
            const std::vector<SecondaryMotionBone>& bones,
            const std::vector<SecondaryMotionSphere>& spheres, const glm::vec3& wind,
            std::vector<glm::mat4>& palette)
        {
            if (bones.empty()) { Reset(); return; }
            if (!std::isfinite(dt) || dt < 0.0f || !Finite(world) ||
                std::abs(glm::determinant(world)) < 1e-8f) { Reset(); return; }
            if (dt == 0.0f) return;
            // A long pause is a discontinuity; start from the current pose.
            // Ordinary low frame rates must advance the entire elapsed time.
            if (dt > 0.25f) { Reset(); dt = 1.0f / 60.0f; }
            mStates.resize(bones.size());
            const auto& boneMap = animation.GetBoneIDMap();
            const glm::mat4 inverseWorld = glm::inverse(world);
            const int steps = std::clamp(static_cast<int>(std::ceil(dt * 120.0f)), 1, 30);
            const float h = dt / steps;
            for (size_t i = 0; i < bones.size(); ++i)
            {
                const auto& settings = bones[i];
                auto& state = mStates[i];
                const auto found = boneMap.find(settings.Bone);
                if (found == boneMap.end() || found->second.id < 0 ||
                    static_cast<size_t>(found->second.id) >= palette.size() ||
                    !Finite(settings.Tail) || !std::isfinite(settings.Stiffness) ||
                    !std::isfinite(settings.Damping) || !std::isfinite(settings.GravityScale) ||
                    !std::isfinite(settings.MotionDrag) ||
                    !std::isfinite(settings.MaxAngleDegrees) || !std::isfinite(settings.Radius))
                { state = {}; continue; }
                const glm::mat4 global = world * palette[found->second.id] * glm::inverse(found->second.offset);
                const glm::vec3 pivot = glm::vec3(global[3]);
                const glm::vec3 rest = glm::vec3(global * glm::vec4(settings.Tail, 1.0f));
                const float length = glm::length(rest - pivot);
                if (!Finite(rest) || length < 1e-5f) { state = {}; continue; }
                const glm::vec3 axis = (rest - pivot) / length;
                // A cut can rotate the rig around an unmoving pivot. Treat a
                // >100 degree single-frame turn as a pose discontinuity.
                if (state.Name != settings.Bone || !state.bInitialized ||
                    glm::distance(state.Pivot, pivot) > 2.0f || !Finite(state.Tip) ||
                    glm::dot(state.RestAxis, axis) < -0.17364818f)
                {
                    state = {};
                    state.Name = settings.Bone;
                    state.Tip = rest;
                    state.Pivot = pivot;
                    state.RestAxis = axis;
                    state.bInitialized = true;
                }
                const float angleLimit = glm::radians(std::clamp(settings.MaxAngleDegrees, 0.0f, 85.0f));
                const auto constrain = [&](glm::vec3 point, const glm::vec3& stepPivot,
                    const glm::vec3& stepAxis)
                {
                    glm::vec3 direction = point - stepPivot;
                    const float distance = glm::length(direction);
                    direction = distance > 1e-6f ? direction / distance : stepAxis;
                    const float cosine = std::clamp(glm::dot(stepAxis, direction), -1.0f, 1.0f);
                    const float angle = std::acos(cosine);
                    if (angle > angleLimit)
                        direction = glm::slerp(glm::quat(1, 0, 0, 0), glm::rotation(stepAxis, direction),
                            angleLimit / angle) * stepAxis;
                    return stepPivot + direction * length;
                };
                const glm::vec3 startPivot = state.Pivot;
                const glm::vec3 startAxis = state.RestAxis;
                // The world-space spring already responds to acceleration. Air
                // resistance also bends a moving strand at constant speed.
                // Cap authored-root speed after discontinuities or tiny dt.
                const glm::vec3 rootVelocity = glm::clamp((pivot - startPivot) / dt,
                    glm::vec3(-20.0f), glm::vec3(20.0f));
                for (int step = 0; step < steps; ++step)
                {
                    const float progress = static_cast<float>(step + 1) / static_cast<float>(steps);
                    const glm::vec3 stepPivot = glm::mix(startPivot, pivot, progress);
                    const glm::vec3 stepAxis = glm::normalize(glm::mix(startAxis, axis, progress));
                    const glm::vec3 stepRest = stepPivot + stepAxis * length;
                    const glm::vec3 previous = state.Tip;
                    const glm::vec3 force = (stepRest - state.Tip) * std::clamp(settings.Stiffness, 0.0f, 400.0f) +
                        glm::vec3(0, -9.81f, 0) * std::clamp(settings.GravityScale, -2.0f, 2.0f) +
                        (Finite(wind) ? glm::clamp(wind, glm::vec3(-20), glm::vec3(20)) : glm::vec3(0)) -
                        rootVelocity * std::clamp(settings.MotionDrag, 0.0f, 10.0f);
                    // Damping belongs in the moving root's frame. Damping
                    // absolute world velocity acts like uncontrolled wind at
                    // constant character speed and pins short strands at their
                    // angle limit even when no airflow was authored.
                    state.Velocity = rootVelocity + (state.Velocity - rootVelocity + force * h) *
                        std::exp(-std::clamp(settings.Damping, 0.0f, 60.0f) * h);
                    state.Tip = constrain(state.Tip + state.Velocity * h, stepPivot, stepAxis);
                    // An authored collider may push a tip a substantial distance in
                    // one substep. Treat that correction as a position constraint,
                    // not as an impulse that launches the hair on the next frame.
                    state.Velocity = (state.Tip - previous) / h;
                    // Re-project a bounded number of times. Bone length and angular limit
                    // take precedence when an authored collider makes constraints infeasible.
                    for (int iteration = 0; iteration < 3; ++iteration)
                        for (const auto& sphere : spheres)
                        {
                            const auto colliderBone = boneMap.find(sphere.Bone);
                            if (colliderBone == boneMap.end() || colliderBone->second.id < 0 ||
                                static_cast<size_t>(colliderBone->second.id) >= palette.size() ||
                                !Finite(sphere.Offset) || !std::isfinite(sphere.Radius) || sphere.Radius <= 0) continue;
                            const glm::mat4 collider = world * palette[colliderBone->second.id] * glm::inverse(colliderBone->second.offset);
                            const glm::vec3 center = glm::vec3(collider * glm::vec4(sphere.Offset, 1));
                            const float scale = std::max({glm::length(glm::vec3(collider[0])),
                                glm::length(glm::vec3(collider[1])), glm::length(glm::vec3(collider[2]))});
                            // Both authored radii use the skeleton's local units.
                            // Applying the object scale to only the body collider
                            // makes a centimetre-scale hair tip enormous in world space.
                            const float radius = (sphere.Radius + std::max(0.0f, settings.Radius)) * scale;
                            glm::vec3 outward = state.Tip - center;
                            const float distance = glm::length(outward);
                            if (distance < radius)
                            {
                                outward = distance > 1e-6f ? outward / distance : stepAxis;
                                state.Tip = constrain(center + outward * radius, stepPivot, stepAxis);
                                const glm::vec3 contact = glm::normalize(state.Tip - center);
                                if (Finite(contact))
                                    state.Velocity -= contact * std::min(glm::dot(state.Velocity, contact), 0.0f);
                            }
                        }
                }
                state.Pivot = pivot;
                state.RestAxis = axis;
                const glm::quat rotation = glm::rotation(axis, glm::normalize(state.Tip - pivot));
                const glm::mat4 delta = inverseWorld * glm::translate(glm::mat4(1), pivot) *
                    glm::toMat4(rotation) * glm::translate(glm::mat4(1), -pivot) * world;
                const auto visit = [&](const auto& self, const AssimpNodeData& node, bool affected) -> void
                {
                    affected = affected || node.name == settings.Bone;
                    const auto bone = boneMap.find(node.name);
                    if (affected && bone != boneMap.end() && bone->second.id >= 0 &&
                        static_cast<size_t>(bone->second.id) < palette.size())
                        palette[bone->second.id] = delta * palette[bone->second.id];
                    for (const auto& child : node.children) self(self, child, affected);
                };
                visit(visit, animation.GetRootNode(), false);
            }
        }

    private:
        static bool Finite(const glm::vec3& value)
        { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
        static bool Finite(const glm::mat4& value)
        {
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    if (!std::isfinite(value[column][row])) return false;
            return true;
        }
        struct State
        {
            std::string Name;
            glm::vec3 Tip = glm::vec3(0);
            glm::vec3 Pivot = glm::vec3(0);
            glm::vec3 RestAxis = glm::vec3(0);
            glm::vec3 Velocity = glm::vec3(0);
            bool bInitialized = false;
        };
        std::vector<State> mStates;
    };
}
