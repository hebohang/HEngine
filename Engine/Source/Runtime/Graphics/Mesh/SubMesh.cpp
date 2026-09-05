#include "Hepch.h"

#include "Runtime/Graphics/Mesh/SubMesh.h"

#include <array>
#include <cmath>
#include <limits>
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
        ComputeBounds(mSkinnedVertices, mBoundsMin, mBoundsMax, bHasBounds);
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
