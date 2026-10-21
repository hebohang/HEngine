#pragma once
// Frozen pre-hoisting solver for trajectory equivalence tests; never used by runtime.
#include "Runtime/Animation/ClothSimulation.h"
namespace HEngine::Tests
{
    class ReferenceClothSimulation
    {
    public:
        void Reset() { mPositions.clear(); mVelocity.clear(); mEdges.clear(); }
        const std::vector<glm::vec3>& Positions() const { return mPositions; }
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
            const glm::vec3 force = glm::vec3(0, -9.81f, 0) +
                (Finite(wind) ? glm::clamp(wind, glm::vec3(-30), glm::vec3(30)) : glm::vec3(0));
            mInverseMass.resize(rest.size());
            auto& inverseMass = mInverseMass;
            for (size_t i = 0; i < rest.size(); ++i)
                inverseMass[i] = std::isfinite(radii[i]) && radii[i] > 0.00001f ? 1.0f : 0.0f;
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
                for (int iteration = 0; iteration < std::clamp(settings.Iterations, 1, 24); ++iteration)
                {
                    for (auto& edge : mEdges)
                    {
                        const glm::vec3 delta = mPositions[edge.B] - mPositions[edge.A];
                        const float length = glm::length(delta);
                        const float mass = inverseMass[edge.A] + inverseMass[edge.B];
                        if (length < 1e-7f || mass == 0) continue;
                        const float compliance = edge.Bend ? settings.BendCompliance : settings.StretchCompliance;
                        const float alpha = Safe(compliance, 0, 1, 0.000001f) / (h * h);
                        const float lambda = (-(length - edge.Length) - alpha * edge.Lambda) / (mass + alpha);
                        edge.Lambda += lambda;
                        const glm::vec3 correction = lambda * delta / length;
                        mPositions[edge.A] -= inverseMass[edge.A] * correction;
                        mPositions[edge.B] += inverseMass[edge.B] * correction;
                    }
                    if (settings.SelfCollision) SolveSelfCollision(inverseMass, thickness);
                    for (size_t i = 0; i < rest.size(); ++i)
                    {
                        if (inverseMass[i] == 0) { mPositions[i] = rest[i]; continue; }
                        // Authored tether is applied before body collision: body clearance
                        // wins when a garment's painted envelope intersects its collider.
                        const glm::vec3 delta = mPositions[i] - rest[i];
                        const float distance = glm::length(delta);
                        const float radius = std::clamp(radii[i], 0.0f, 3.0f);
                        if (distance > radius) mPositions[i] = rest[i] + delta * (radius / distance);
                        for (const auto& capsule : capsules)
                        {
                            if (!Finite(capsule.Start) || !Finite(capsule.End) ||
                                !std::isfinite(capsule.Radius) || capsule.Radius <= 0) continue;
                            const glm::vec3 axis = capsule.End - capsule.Start;
                            const float axisSquared = glm::dot(axis, axis);
                            const float t = axisSquared > 1e-9f ? std::clamp(glm::dot(mPositions[i] - capsule.Start, axis) / axisSquared, 0.0f, 1.0f) : 0;
                            const glm::vec3 center = capsule.Start + t * axis;
                            glm::vec3 outward = mPositions[i] - center;
                            const float length = glm::length(outward);
                            const float collisionRadius = capsule.Radius + thickness;
                            if (length < collisionRadius)
                            {
                                if (length < 1e-7f)
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
                    const float speed = glm::length(mVelocity[i]);
                    if (speed > 15) mVelocity[i] *= 15 / speed;
                }
            }
        }

    private:
        struct Edge { uint32_t A, B; float Length; bool Bend; float Lambda = 0; };
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
        std::vector<Edge> mEdges;
        glm::vec3 mOrigin{0};
        bool bReset = false;
    };

}
