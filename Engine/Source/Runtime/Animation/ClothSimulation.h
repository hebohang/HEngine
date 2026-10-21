#pragma once

#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace HEngine
{
    REFLECTION_TYPE(ClothPanel)
    STRUCT(ClothPanel, Fields)
    {
        REFLECTION_BODY(ClothPanel);
        // Vertex colour red paints the allowed motion radius: zero pins to skin.
        int MaterialIndex = -1;
        float MaxDistance = 0.35f;
        float StretchCompliance = 0.000001f;
        float BendCompliance = 0.001f;
        float Damping = 2.0f;
        float Thickness = 0.008f;
        int Iterations = 8;
        bool SelfCollision = false;
    };

    REFLECTION_TYPE(ClothCapsule)
    STRUCT(ClothCapsule, Fields)
    {
        REFLECTION_BODY(ClothCapsule);
        std::string Bone;
        glm::vec3 Start = glm::vec3(0);
        glm::vec3 End = glm::vec3(0, 0.3f, 0);
        float Radius = 0.15f;
    };

    struct ClothWorldCapsule
    {
        glm::vec3 Start{0};
        glm::vec3 End{0};
        float Radius = 0;
    };

    // Bounded CPU XPBD for small hero garments. Positions are world-space;
    // callers own skinning, topology and per-instance render publication.
    class ClothSimulation
    {
    public:
        void Reset()
        {
            mPositions.clear();
            mVelocity.clear();
            mEdges.clear();
            mCapsuleCandidateOffsets.clear();
            mCapsuleCandidates.clear();
            mMovableParticles.clear();
            mSolvableEdges.clear();
        }
        const std::vector<glm::vec3>& Positions() const { return mPositions; }
        const std::vector<glm::vec3>& Velocities() const { return mVelocity; }
        bool WasReset() const { return bReset; }

        void Step(float dt, const std::vector<glm::vec3>& rest,
            const std::vector<uint32_t>& triangles, const std::vector<float>& radii,
            const std::vector<ClothWorldCapsule>& capsules, const glm::vec3& wind,
            const ClothPanel& settings, const glm::vec3& origin)
        {
            bReset = false;
            if (rest.empty() || radii.size() != rest.size() || !Finite(origin) ||
                !std::isfinite(dt) || dt < 0 ||
                std::any_of(rest.begin(), rest.end(), [](auto p) { return !Finite(p); }))
            { Reset(); return; }
            const bool reset = mPositions.size() != rest.size() || dt > 0.25f ||
                glm::distance(origin, mOrigin) > 2.0f;
            mOrigin = origin;
            if (reset)
            {
                mPositions = rest;
                mVelocity.assign(rest.size(), glm::vec3(0));
                BuildEdges(rest, triangles);
                bReset = true;
                return;
            }
            // Tiny positive timesteps can underflow h*h in XPBD compliance.
            // Treat them like a paused frame, retaining positions and velocity.
            if (dt < 1e-6f) return;
            const float duration = std::min(dt, 1.0f / 15.0f);
            const int steps = std::clamp(static_cast<int>(std::ceil(duration * 120)), 1, 8);
            const float h = duration / steps;
            const float damping = std::exp(-Safe(settings.Damping, 0, 30, 2) * h);
            const float thickness = Safe(settings.Thickness, 0.001f, 0.1f, 0.008f);
            const float stretchAlpha = Safe(settings.StretchCompliance, 0, 1, 0.000001f) / (h * h);
            const float bendAlpha = Safe(settings.BendCompliance, 0, 1, 0.000001f) / (h * h);
            const int iterations = std::clamp(settings.Iterations, 1, 24);
            mPreparedCapsules.clear();
            mPreparedCapsules.reserve(capsules.size());
            for (const auto& capsule : capsules)
            {
                if (!Finite(capsule.Start) || !Finite(capsule.End) ||
                    !std::isfinite(capsule.Radius) || capsule.Radius <= 0) continue;
                const glm::vec3 axis = capsule.End - capsule.Start;
                const float collisionRadius = capsule.Radius + thickness;
                mPreparedCapsules.push_back({capsule.Start, axis, glm::dot(axis, axis), collisionRadius,
                    glm::min(capsule.Start, capsule.End) - glm::vec3(collisionRadius),
                    glm::max(capsule.Start, capsule.End) + glm::vec3(collisionRadius)});
            }
            const glm::vec3 force = glm::vec3(0, -9.81f, 0) +
                (Finite(wind) ? glm::clamp(wind, glm::vec3(-30), glm::vec3(30)) : glm::vec3(0));
            mInverseMass.resize(rest.size());
            mTetherRadii.resize(rest.size());
            mCapsuleCandidateOffsets.resize(rest.size() + 1);
            mCapsuleCandidates.clear();
            mMovableParticles.clear();
            auto& inverseMass = mInverseMass;
            for (size_t i = 0; i < rest.size(); ++i)
            {
                inverseMass[i] = std::isfinite(radii[i]) && radii[i] > 0.00001f ? 1.0f : 0.0f;
                mTetherRadii[i] = inverseMass[i] > 0 ? std::clamp(radii[i], 0.0f, 3.0f) : 0.0f;
                mCapsuleCandidateOffsets[i] = mCapsuleCandidates.size();
                if (inverseMass[i] == 0) continue;
                mMovableParticles.push_back(i);
                // A tethered particle cannot reach a capsule whose expanded
                // bounds do not contain its current skinned rest position.
                const float radius = mTetherRadii[i];
                for (size_t capsuleIndex = 0; capsuleIndex < mPreparedCapsules.size(); ++capsuleIndex)
                {
                    const auto& capsule = mPreparedCapsules[capsuleIndex];
                    const glm::vec3& point = rest[i];
                    if (point.x < capsule.BoundsMin.x - radius || point.x > capsule.BoundsMax.x + radius ||
                        point.y < capsule.BoundsMin.y - radius || point.y > capsule.BoundsMax.y + radius ||
                        point.z < capsule.BoundsMin.z - radius || point.z > capsule.BoundsMax.z + radius) continue;
                    mCapsuleCandidates.push_back(capsuleIndex);
                }
            }
            mCapsuleCandidateOffsets[rest.size()] = mCapsuleCandidates.size();
            // Compliance and inverse mass stay fixed through all iterations
            // of this frame. Resolve their denominator once per edge while
            // retaining the reference solver's division order.
            mSolvableEdges.clear();
            for (size_t edgeIndex = 0; edgeIndex < mEdges.size(); ++edgeIndex)
            {
                auto& edge = mEdges[edgeIndex];
                edge.Alpha = edge.Bend ? bendAlpha : stretchAlpha;
                const float mass = inverseMass[edge.A] + inverseMass[edge.B];
                edge.Denominator = mass > 0.0f ? mass + edge.Alpha : 0.0f;
                if (edge.Denominator != 0.0f) mSolvableEdges.push_back(edgeIndex);
            }
            for (int substep = 0; substep < steps; ++substep)
            {
                mPreviousPositions = mPositions;
                const auto& previous = mPreviousPositions;
                for (size_t i = 0; i < rest.size(); ++i)
                {
                    if (inverseMass[i] == 0) { mPositions[i] = rest[i]; mVelocity[i] = glm::vec3(0); }
                    else
                    {
                        mVelocity[i] = (mVelocity[i] + force * h) * damping;
                        mPositions[i] += mVelocity[i] * h;
                    }
                }
                for (auto& edge : mEdges) edge.Lambda = 0;
                for (int iteration = 0; iteration < iterations; ++iteration)
                {
                    for (const size_t edgeIndex : mSolvableEdges)
                    {
                        auto& edge = mEdges[edgeIndex];
                        const glm::vec3 delta = mPositions[edge.B] - mPositions[edge.A];
                        const float length = glm::length(delta);
                        if (length < 1e-7f) continue;
                        const float lambda = (-(length - edge.Length) - edge.Alpha * edge.Lambda) /
                            edge.Denominator;
                        edge.Lambda += lambda;
                        const glm::vec3 correction = lambda * delta / length;
                        mPositions[edge.A] -= inverseMass[edge.A] * correction;
                        mPositions[edge.B] += inverseMass[edge.B] * correction;
                    }
                    if (settings.SelfCollision) SolveSelfCollision(inverseMass, thickness);
                    for (const size_t i : mMovableParticles)
                    {
                        // Authored tether is applied before body collision: body clearance
                        // wins when a garment's painted envelope intersects its collider.
                        const glm::vec3 delta = mPositions[i] - rest[i];
                        const float radius = mTetherRadii[i];
                        if (glm::dot(delta, delta) > radius * radius)
                            mPositions[i] = rest[i] + delta * (radius / glm::length(delta));
                        for (size_t candidate = mCapsuleCandidateOffsets[i];
                             candidate < mCapsuleCandidateOffsets[i + 1]; ++candidate)
                        {
                            const auto& capsule = mPreparedCapsules[mCapsuleCandidates[candidate]];
                            const glm::vec3& point = mPositions[i];
                            if (point.x < capsule.BoundsMin.x || point.x > capsule.BoundsMax.x ||
                                point.y < capsule.BoundsMin.y || point.y > capsule.BoundsMax.y ||
                                point.z < capsule.BoundsMin.z || point.z > capsule.BoundsMax.z) continue;
                            const float t = capsule.AxisSquared > 1e-9f ? std::clamp(glm::dot(mPositions[i] - capsule.Start, capsule.Axis) / capsule.AxisSquared, 0.0f, 1.0f) : 0;
                            const glm::vec3 center = capsule.Start + t * capsule.Axis;
                            glm::vec3 outward = mPositions[i] - center;
                            const float collisionRadius = capsule.CollisionRadius;
                            const float lengthSquared = glm::dot(outward, outward);
                            if (lengthSquared < collisionRadius * collisionRadius)
                            {
                                if (lengthSquared < 1e-14f)
                                {
                                    outward = rest[i] - center;
                                    if (glm::length(outward) < 1e-7f) outward = glm::vec3(1, 0, 0);
                                }
                                mPositions[i] = center + glm::normalize(outward) * collisionRadius;
                            }
                        }
                    }
                }
                for (size_t i = 0; i < rest.size(); ++i)
                {
                    if (!Finite(mPositions[i])) { Reset(); bReset = true; return; }
                    mVelocity[i] = inverseMass[i] > 0 ? (mPositions[i] - previous[i]) / h : glm::vec3(0);
                    if (glm::dot(mVelocity[i], mVelocity[i]) > 225.0f)
                        mVelocity[i] *= 15.0f / glm::length(mVelocity[i]);
                }
            }
        }

    private:
        struct Edge { uint32_t A, B; float Length; bool Bend; float Lambda = 0; float Alpha = 0; float Denominator = 0; };
        static bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        static float Safe(float value, float low, float high, float fallback)
        { return std::isfinite(value) ? std::clamp(value, low, high) : fallback; }
        void BuildEdges(const std::vector<glm::vec3>& rest, const std::vector<uint32_t>& triangles)
        {
            mEdges.clear();
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> edges;
            for (size_t i = 0; i + 2 < triangles.size(); i += 3)
                for (int j = 0; j < 3; ++j)
                {
                    const uint32_t a = triangles[i + j], b = triangles[i + (j + 1) % 3], opposite = triangles[i + (j + 2) % 3];
                    if (a >= rest.size() || b >= rest.size() || opposite >= rest.size() || a == b) continue;
                    const auto key = std::make_pair(std::min(a, b), std::max(a, b));
                    const auto [entry, inserted] = edges.emplace(key, opposite);
                    if (inserted) mEdges.push_back({a, b, glm::distance(rest[a], rest[b]), false});
                    else if (opposite != entry->second)
                        mEdges.push_back({opposite, entry->second, glm::distance(rest[opposite], rest[entry->second]), true});
                }
        }
        void SolveSelfCollision(const std::vector<float>& mass, float thickness)
        {
            // Spatial bins bound work for small panels. Author particle thickness
            // below half the mesh spacing so neighbours do not repel at rest.
            const float diameter = thickness * 2;
            using Cell = std::tuple<int, int, int>;
            std::map<Cell, std::vector<size_t>> cells;
            for (size_t i = 0; i < mPositions.size(); ++i)
            {
                // Hash relative to the actor, not the world origin. Do the
                // subtraction in double and validate before integer conversion;
                // huge finite coordinates and failed constraints must never cause
                // undefined float-to-int conversion or neighbour-index overflow.
                glm::ivec3 cell(0);
                bool validCell = true;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const double coordinate = std::floor((static_cast<double>(mPositions[i][axis]) -
                        static_cast<double>(mOrigin[axis])) / static_cast<double>(diameter));
                    if (!std::isfinite(coordinate) ||
                        coordinate < static_cast<double>(std::numeric_limits<int>::min()) + 1.0 ||
                        coordinate > static_cast<double>(std::numeric_limits<int>::max()) - 1.0)
                    { validCell = false; break; }
                    cell[axis] = static_cast<int>(coordinate);
                }
                // Out-of-range particles cannot form a representable local grid
                // cell. Non-finite simulation state is reset by Step below.
                if (!validCell) continue;
                for (int z = -1; z <= 1; ++z) for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
                {
                    const auto found = cells.find({cell.x + x, cell.y + y, cell.z + z});
                    if (found == cells.end()) continue;
                    for (const size_t j : found->second)
                    {
                        const float total = mass[i] + mass[j];
                        const glm::vec3 delta = mPositions[i] - mPositions[j];
                        const float distance = glm::length(delta);
                        if (total == 0 || distance >= diameter || distance < 1e-7f) continue;
                        const glm::vec3 push = delta * ((diameter - distance) / (distance * total));
                        mPositions[i] += mass[i] * push;
                        mPositions[j] -= mass[j] * push;
                    }
                }
                cells[{cell.x, cell.y, cell.z}].push_back(i);
            }
        }
        std::vector<glm::vec3> mPositions, mVelocity;
        std::vector<glm::vec3> mPreviousPositions;
        std::vector<float> mInverseMass;
        std::vector<float> mTetherRadii;
        std::vector<size_t> mCapsuleCandidateOffsets, mCapsuleCandidates;
        std::vector<size_t> mMovableParticles, mSolvableEdges;
        struct PreparedCapsule { glm::vec3 Start, Axis; float AxisSquared, CollisionRadius; glm::vec3 BoundsMin, BoundsMax; };
        std::vector<PreparedCapsule> mPreparedCapsules;
        std::vector<Edge> mEdges;
        glm::vec3 mOrigin{0};
        bool bReset = false;
    };

    // Render vertices split at UV/normal seams; the simulation must retain one
    // particle per coincident bind-space position. Cache lives with the entity
    // and is discarded with its solver on reload, stop or distance opt-out.
    struct ClothRenderState : ClothSimulation
    {
        const void* BindVertexData = nullptr;
        const void* IndexData = nullptr;
        size_t IndexCount = 0;
        uint32_t ParticleCount = 0;
        std::vector<uint32_t> RenderToParticle;
        // Highest occupied influence slot, cached with the source topology.
        // Gaps remain supported; only guaranteed empty trailing slots are skipped.
        std::vector<uint8_t> InfluenceCounts;
        std::vector<uint32_t> SimulationTriangles;
        std::vector<glm::vec3> Rest, RenderPositions, Normals, Tangents;
        std::vector<uint32_t> Contributors;
        std::vector<float> Radii;
        std::vector<glm::mat4> InverseSkins;
        std::vector<glm::mat3> NormalToLocal;
    };
}
