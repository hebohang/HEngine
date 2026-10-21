#include "Hepch.h"

#include "Runtime/Graphics/Mesh/SubMesh.h"

#include "Runtime/Core/TaskScheduler.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        template<typename Vertex>
        void ComputeBounds(
            const std::vector<Vertex>& vertices,
            glm::vec3& boundsMin,
            glm::vec3& boundsMax,
            bool& bHasBounds)
        {
            if (vertices.empty())
                return;

            boundsMin = glm::vec3(std::numeric_limits<float>::max());
            boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
            for (const Vertex& vertex : vertices)
            {
                boundsMin = glm::min(boundsMin, vertex.Pos);
                boundsMax = glm::max(boundsMax, vertex.Pos);
            }
            bHasBounds = true;
        }

        std::array<glm::vec4, 6> ExtractFrustumPlanes(const glm::mat4& viewProjection)
        {
            const glm::vec4 row0(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]);
            const glm::vec4 row1(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]);
            const glm::vec4 row2(viewProjection[0][2], viewProjection[1][2], viewProjection[2][2], viewProjection[3][2]);
            const glm::vec4 row3(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);

            return {
                row3 + row0,
                row3 - row0,
                row3 + row1,
                row3 - row1,
                row3 + row2,
                row3 - row2 };
        }
    }

    SubMesh::SubMesh(
        std::vector<StaticVertex> vertices,
        std::vector<uint32_t> indices,
        uint32_t materialIndex)
        : mMaterialIndex(materialIndex)
        , mStaticVertices(std::move(vertices))
        , mIndices(std::move(indices))
    {
        for (auto& vertex : mStaticVertices) vertex.PreviousPosition = vertex.Pos;
        ComputeBounds(mStaticVertices, mBoundsMin, mBoundsMax, bHasBounds);
    }

    SubMesh::SubMesh(
        std::vector<SkinnedVertex> vertices,
        std::vector<uint32_t> indices,
        uint32_t materialIndex)
        : mMaterialIndex(materialIndex)
        , mSkinnedVertices(std::move(vertices))
        , mIndices(std::move(indices))
    {
        for (auto& vertex : mSkinnedVertices) vertex.PreviousPosition = vertex.Pos;
        ComputeBounds(mSkinnedVertices, mBoundsMin, mBoundsMax, bHasBounds);
    }

    void SubMesh::SetMorphTargets(std::vector<MorphTarget> targets)
    {
        const size_t count = IsSkinned() ? mSkinnedVertices.size() : mStaticVertices.size();
        if (targets.size() > 256) throw std::invalid_argument("Morph target count exceeds 256");
        for (const auto& target : targets)
        {
            if (target.Name.empty() || !std::isfinite(target.DefaultWeight))
                throw std::invalid_argument("Invalid morph target name or default weight");
            for (const auto* stream : { &target.Positions, &target.Normals, &target.Tangents })
            {
                if (!stream->empty() && stream->size() != count)
                    throw std::invalid_argument("Morph target vertex count mismatch");
                for (const auto& v : *stream)
                    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                        throw std::invalid_argument("Non-finite morph target delta");
            }
        }
        mMorphTargetAffectsGeometry.clear();
        mMorphTargetAffectsGeometry.reserve(targets.size());
        mHasEffectiveMorphTargets = false;
        const auto hasDelta = [](const std::vector<glm::vec3>& stream)
        {
            return std::any_of(stream.begin(), stream.end(), [](const glm::vec3& delta)
            {
                return delta.x != 0.0f || delta.y != 0.0f || delta.z != 0.0f;
            });
        };
        for (const auto& target : targets)
        {
            const bool affectsGeometry = hasDelta(target.Positions) ||
                hasDelta(target.Normals) || hasDelta(target.Tangents);
            mMorphTargetAffectsGeometry.push_back(affectsGeometry);
            mHasEffectiveMorphTargets |= affectsGeometry;
        }
        mMorphTargets = std::move(targets);
        mMorphCacheOwner = nullptr;
    }

    void SubMesh::BeginDeformation()
    {
        mMorphCacheOwner = nullptr;
        if (IsSkinned())
        {
            if (mBaseSkinnedVertices.empty()) mBaseSkinnedVertices = mSkinnedVertices;
            for (size_t i = 0; i < mSkinnedVertices.size(); ++i)
            {
                const glm::vec3 previous = mSkinnedVertices[i].Pos;
                mSkinnedVertices[i] = mBaseSkinnedVertices[i];
                mSkinnedVertices[i].PreviousPosition = previous;
            }
        }
        else
        {
            if (mBaseStaticVertices.empty()) mBaseStaticVertices = mStaticVertices;
            for (size_t i = 0; i < mStaticVertices.size(); ++i)
            {
                const glm::vec3 previous = mStaticVertices[i].Pos;
                mStaticVertices[i] = mBaseStaticVertices[i];
                mStaticVertices[i].PreviousPosition = previous;
            }
        }
    }

    void SubMesh::ApplyMorphWeights(const std::map<std::string, float>& weights)
    {
        if (!mHasEffectiveMorphTargets) return;
        struct ActiveMorph { const MorphTarget* Target; float Weight; };
        std::vector<ActiveMorph> activeMorphs;
        activeMorphs.reserve(mMorphTargets.size());
        for (size_t targetIndex = 0; targetIndex < mMorphTargets.size(); ++targetIndex)
        {
            if (!mMorphTargetAffectsGeometry[targetIndex]) continue;
            const auto& target = mMorphTargets[targetIndex];
            const auto found = weights.find(target.Name);
            const float raw = found == weights.end() ? target.DefaultWeight : found->second;
            const float weight = std::isfinite(raw) ? std::clamp(raw, -2.0f, 2.0f) : 0.0f;
            if (std::abs(weight) >= 1e-6f)
                activeMorphs.push_back({ &target, weight });
        }
        const auto apply = [&](auto& vertices)
        {
            const auto applyVertex = [&](size_t i)
            {
                auto& vertex = vertices[i];
                for (const ActiveMorph& morph : activeMorphs)
                {
                    const auto& target = *morph.Target;
                    if (!target.Positions.empty()) vertex.Pos += target.Positions[i] * morph.Weight;
                    if (!target.Normals.empty()) vertex.Normal += target.Normals[i] * morph.Weight;
                    if (!target.Tangents.empty()) vertex.Tangent += target.Tangents[i] * morph.Weight;
                }
                if (glm::dot(vertex.Normal, vertex.Normal) > 1e-10f) vertex.Normal = glm::normalize(vertex.Normal);
                const float sign = glm::dot(glm::cross(vertex.Normal, vertex.Tangent), vertex.Bitangent) < 0.0f ? -1.0f : 1.0f;
                vertex.Tangent -= vertex.Normal * glm::dot(vertex.Normal, vertex.Tangent);
                if (glm::dot(vertex.Tangent, vertex.Tangent) > 1e-10f) vertex.Tangent = glm::normalize(vertex.Tangent);
                vertex.Bitangent = glm::cross(vertex.Normal, vertex.Tangent) * sign;
            };
            static const bool bForceSerial = []
            {
                const char* setting = std::getenv("HE_MORPH_TASKS_SERIAL");
                return setting && setting[0] == '1' && setting[1] == '\0';
            }();
            if (!bForceSerial && vertices.size() >= 65536 && activeMorphs.size() >= 2)
                (void)GetRuntimeTaskScheduler().ParallelFor(
                    "Morph vertices", vertices.size(), 8192, applyVertex);
            else
            {
                for (size_t i = 0; i < vertices.size(); ++i)
                    applyVertex(i);
            }
        };
        if (IsSkinned()) apply(mSkinnedVertices); else apply(mStaticVertices);
    }

    void SubMesh::EndDeformation(bool resetHistory)
    {
        mMorphCacheOwner = nullptr;
        if (resetHistory)
        {
            ++mDeformationHistoryGeneration;
            for (auto& vertex : mSkinnedVertices) vertex.PreviousPosition = vertex.Pos;
            for (auto& vertex : mStaticVertices) vertex.PreviousPosition = vertex.Pos;
        }
        if (IsSkinned()) ComputeBounds(mSkinnedVertices, mBoundsMin, mBoundsMax, bHasBounds);
        else ComputeBounds(mStaticVertices, mBoundsMin, mBoundsMax, bHasBounds);
        ++mDeformationRevision;
    }

    void SubMesh::UpdatePureMorphWeights(const std::map<std::string, float>& weights, bool resetHistory)
    {
        if (!mHasEffectiveMorphTargets && mDeformationRevision == 0) return;
        const auto effective = [&](const MorphTarget& target) {
            const auto found = weights.find(target.Name);
            const float raw = found == weights.end() ? target.DefaultWeight : found->second;
            const float value = std::isfinite(raw) ? std::clamp(raw, -2.0f, 2.0f) : 0.0f;
            return std::abs(value) < 1e-6f ? 0.0f : value;
        };
        bool unchanged = mMorphCacheOwner == this && mCachedMorphWeights.size() == mMorphTargets.size();
        for (size_t i = 0; unchanged && i < mMorphTargets.size(); ++i)
            if (mMorphTargetAffectsGeometry[i])
                unchanged = mCachedMorphWeights[i] == effective(mMorphTargets[i]);
        if (unchanged)
        {
            if (resetHistory) ++mDeformationHistoryGeneration;
            if (bMorphHistoryPending)
            {
                for (auto& vertex : mSkinnedVertices) vertex.PreviousPosition = vertex.Pos;
                for (auto& vertex : mStaticVertices) vertex.PreviousPosition = vertex.Pos;
                ++mDeformationRevision;
                bMorphHistoryPending = false;
            }
            // Views observe reset generation even when this simulation stream
            // has settled: a view may still retain an older submitted pose.
            return;
        }
        BeginDeformation();
        ApplyMorphWeights(weights);
        EndDeformation(resetHistory);
        mCachedMorphWeights.resize(mMorphTargets.size());
        for (size_t i = 0; i < mMorphTargets.size(); ++i) mCachedMorphWeights[i] = effective(mMorphTargets[i]);
        mMorphCacheOwner = this; // Copied sections intentionally fail this identity check.
        bMorphHistoryPending = !resetHistory;
    }

    void SubMesh::SetLods(std::vector<SubMeshLod> lods)
    {
        if (lods.size() > 8) throw std::invalid_argument("SubMesh supports at most eight authored LODs");
        const size_t vertexCount = IsSkinned() ? mSkinnedVertices.size() : mStaticVertices.size();
        size_t previousCount = mIndices.size();
        float previousError = 0.0f;
        for (const auto& lod : lods)
        {
            if (lod.Indices.empty() || lod.Indices.size() % 3 != 0 || lod.Indices.size() >= previousCount ||
                !std::isfinite(lod.ObjectSpaceError) || lod.ObjectSpaceError < previousError)
                throw std::invalid_argument("Invalid SubMesh LOD topology/error ordering");
            for (uint32_t index : lod.Indices)
                if (index >= vertexCount) throw std::out_of_range("SubMesh LOD index exceeds vertex count");
            previousCount = lod.Indices.size();
            previousError = lod.ObjectSpaceError;
        }
        mLods = std::move(lods);
    }

    bool SubMesh::IsVisible(const glm::mat4& transform, const glm::mat4& viewProjection) const
    {
        if (!bHasBounds)
            return true;

        const glm::vec3 localCenter = (mBoundsMin + mBoundsMax) * 0.5f;
        const glm::vec3 localExtents = (mBoundsMax - mBoundsMin) * 0.5f;
        const glm::vec3 worldCenter = glm::vec3(transform * glm::vec4(localCenter, 1.0f));
        const glm::mat3 worldBasis(transform);
        const glm::vec3 worldExtents =
            glm::abs(worldBasis[0]) * localExtents.x +
            glm::abs(worldBasis[1]) * localExtents.y +
            glm::abs(worldBasis[2]) * localExtents.z;

        const auto bIsFinite = [](const glm::vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        if (!bIsFinite(worldCenter) || !bIsFinite(worldExtents))
            return true;

        for (const glm::vec4& plane : ExtractFrustumPlanes(viewProjection))
        {
            const glm::vec3 normal(plane);
            const float projectedRadius = glm::dot(glm::abs(normal), worldExtents);
            const float signedDistance = glm::dot(normal, worldCenter) + plane.w;
            if (signedDistance + projectedRadius < -1e-4f)
                return false;
        }

        return true;
    }
}
