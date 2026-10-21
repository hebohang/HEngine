#include "Hepch.h"

#include "Runtime/Graphics/RayTracing/HardwareRayTracing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr uint64_t FnvOffset = 14695981039346656037ull;
        constexpr uint64_t FnvPrime = 1099511628211ull;

        void HashBytes(uint64_t& hash, const void* data, size_t size) noexcept
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= FnvPrime;
            }
        }

        void HashString(uint64_t& hash, std::string_view value) noexcept
        {
            HashBytes(hash, value.data(), value.size());
            constexpr uint8_t Terminator = 0xff;
            HashBytes(hash, &Terminator, sizeof(Terminator));
        }

        void HashValue(uint64_t& hash, uint64_t value) noexcept
        {
            HashBytes(hash, &value, sizeof(value));
        }

        [[nodiscard]] bool HasOnlyFlatTriangleNormals(
            const SubMesh& subMesh) noexcept
        {
            const std::vector<StaticVertex>& vertices = subMesh.mStaticVertices;
            const std::vector<uint32_t>& indices = subMesh.GetIndices();
            if (vertices.empty() || indices.empty() || indices.size() % 3u != 0u)
                return false;
            for (size_t index = 0; index < indices.size(); index += 3u)
            {
                const uint32_t index0 = indices[index + 0u];
                const uint32_t index1 = indices[index + 1u];
                const uint32_t index2 = indices[index + 2u];
                if (index0 >= vertices.size() || index1 >= vertices.size() ||
                    index2 >= vertices.size())
                {
                    return false;
                }
                const glm::vec3 crossValue = glm::cross(
                    vertices[index1].Pos - vertices[index0].Pos,
                    vertices[index2].Pos - vertices[index0].Pos);
                const float crossLengthSquared = glm::dot(crossValue, crossValue);
                if (!std::isfinite(crossLengthSquared) ||
                    crossLengthSquared <= 1e-16f)
                {
                    return false;
                }
                const glm::vec3 faceNormal = crossValue /
                    std::sqrt(crossLengthSquared);
                for (uint32_t vertexIndex : { index0, index1, index2 })
                {
                    const glm::vec3 normal = vertices[vertexIndex].Normal;
                    const float normalLengthSquared = glm::dot(normal, normal);
                    if (!std::isfinite(normalLengthSquared) ||
                        normalLengthSquared <= 1e-16f ||
                        std::abs(glm::dot(
                            normal / std::sqrt(normalLengthSquared),
                            faceNormal)) < 0.999f)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    HardwareRayTracingSupport QueryHardwareRayTracingSupport(
        const RenderCapabilities& capabilities)
    {
        HardwareRayTracingSupport support;
        support.bAccelerationStructures =
            capabilities.RayTracingAccelStruct.bEnabled;
        support.bPipelines = capabilities.RayTracingPipeline.bEnabled;
        support.bRayQuery = capabilities.RayQuery.bEnabled;
        support.bRealtimeRtgi =
            support.bAccelerationStructures && support.bPipelines;
        support.Reason = support.bRealtimeRtgi
            ? "NVRHI exposes hardware ray-tracing acceleration structures and pipelines."
            : "NVRHI does not expose both hardware ray-tracing acceleration structures and pipelines.";
        return support;
    }

    const char* ToString(RayTracingGeometrySupport support) noexcept
    {
        switch (support)
        {
        case RayTracingGeometrySupport::SupportedStaticOpaque:
            return "supported static opaque";
        case RayTracingGeometrySupport::MissingGeometry:
            return "missing geometry";
        case RayTracingGeometrySupport::Skinned:
            return "skinned geometry";
        case RayTracingGeometrySupport::SmoothShadingNormals:
            return "smooth shading normals";
        case RayTracingGeometrySupport::AlphaMask:
            return "alpha-mask material";
        case RayTracingGeometrySupport::AlphaBlend:
            return "alpha-blend material";
        case RayTracingGeometrySupport::CustomMaterialShader:
            return "custom material shader";
        case RayTracingGeometrySupport::TexturedMaterial:
            return "textured material";
        case RayTracingGeometrySupport::ProceduralGeometry:
            return "procedural geometry";
        case RayTracingGeometrySupport::Vfx:
            return "VFX geometry";
        }
        return "unknown";
    }

    RayTracingGeometrySupport ClassifyRayTracingGeometry(
        const RenderObject& object,
        const RenderMeshSection& section, bool bBasePbrMaterials) noexcept
    {
        if (!section.SubMeshResource)
            return RayTracingGeometrySupport::MissingGeometry;
        if (object.Path == RenderPath::ThirdPartyVfx)
            return RayTracingGeometrySupport::Vfx;
        if (section.SubMeshResource->IsSkinned() ||
            object.Path == RenderPath::CpuSkinned ||
            object.Path == RenderPath::CpuSkinnedTransparent)
        {
            return RayTracingGeometrySupport::Skinned;
        }
        if (!bBasePbrMaterials && !HasOnlyFlatTriangleNormals(*section.SubMeshResource))
            return RayTracingGeometrySupport::SmoothShadingNormals;
        if (section.GetMaterialInstance().bCustomSurface)
            return RayTracingGeometrySupport::CustomMaterialShader;
        if (!bBasePbrMaterials && std::ranges::any_of(
            section.GetMaterialInstance().LegacyTextures,
            [](const RenderMeshSection::MaterialInstanceSnapshot::TextureReference& texture)
            {
                return texture.bEnabled;
            }))
        {
            // The first vertical slice carries only constant BasePBR factors.
            // Until hit shaders consume the bindless material texture table,
            // admitting a textured mesh would silently trace the wrong BSDF.
            return RayTracingGeometrySupport::TexturedMaterial;
        }
        if (!bBasePbrMaterials && section.AlphaMode == RenderMaterialAlphaMode::Mask)
            return RayTracingGeometrySupport::AlphaMask;
        if (section.AlphaMode == RenderMaterialAlphaMode::Blend ||
            object.Path == RenderPath::Transparent)
        {
            return RayTracingGeometrySupport::AlphaBlend;
        }
        return RayTracingGeometrySupport::SupportedStaticOpaque;
    }

    uint64_t ComputeRayTracingGeometryContentRevision(
        const SubMesh& subMesh) noexcept
    {
        uint64_t hash = FnvOffset;
        const bool bSkinned = subMesh.IsSkinned();
        HashBytes(hash, &bSkinned, sizeof(bSkinned));
        if (bSkinned)
        {
            HashBytes(
                hash,
                subMesh.mSkinnedVertices.data(),
                subMesh.mSkinnedVertices.size() * sizeof(SkinnedVertex));
        }
        else
        {
            HashBytes(
                hash,
                subMesh.mStaticVertices.data(),
                subMesh.mStaticVertices.size() * sizeof(StaticVertex));
        }
        const std::vector<uint32_t>& indices = subMesh.GetIndices();
        HashBytes(hash, indices.data(), indices.size() * sizeof(uint32_t));
        return hash == 0 ? 1 : hash;
    }

    uint64_t MemoizeRayTracingGeometryContentRevision(
        RayTracingGeometryContentRevisionMap& revisions,
        const SubMesh& subMesh) noexcept
    {
        const auto existing = revisions.find(&subMesh);
        if (existing != revisions.end())
            return existing->second;
        const uint64_t revision =
            ComputeRayTracingGeometryContentRevision(subMesh);
        revisions.emplace(&subMesh, revision);
        return revision;
    }

    RayTracingGeometryCacheKey MakeRayTracingGeometryCacheKey(
        std::string_view meshAssetPath,
        uint32_t sectionIndex,
        const SubMesh& subMesh,
        RenderMaterialAlphaMode alphaMode,
        bool bOpaque,
        bool bDoubleSided)
    {
        return MakeRayTracingGeometryCacheKey(
            meshAssetPath,
            sectionIndex,
            subMesh,
            ComputeRayTracingGeometryContentRevision(subMesh),
            alphaMode,
            bOpaque,
            bDoubleSided);
    }

    RayTracingGeometryCacheKey MakeRayTracingGeometryCacheKey(
        std::string_view meshAssetPath,
        uint32_t sectionIndex,
        const SubMesh& subMesh,
        uint64_t contentRevision,
        RenderMaterialAlphaMode alphaMode,
        bool bOpaque,
        bool bDoubleSided)
    {
        return {
            .MeshAssetPath = std::string(meshAssetPath),
            .SectionIndex = sectionIndex,
            .ContentRevision = contentRevision,
            .VertexStride = static_cast<uint32_t>(
                subMesh.IsSkinned() ? sizeof(SkinnedVertex) : sizeof(StaticVertex)),
            .PositionOffset = 0,
            .VertexCount = static_cast<uint32_t>(subMesh.IsSkinned()
                ? subMesh.mSkinnedVertices.size()
                : subMesh.mStaticVertices.size()),
            .IndexCount = static_cast<uint32_t>(subMesh.GetIndices().size()),
            .AlphaMode = alphaMode,
            .bOpaque = bOpaque,
            .bDoubleSided = bDoubleSided,
        };
    }

    size_t RayTracingGeometryCacheKeyHash::operator()(
        const RayTracingGeometryCacheKey& key) const noexcept
    {
        uint64_t hash = FnvOffset;
        HashString(hash, key.MeshAssetPath);
        HashValue(hash, key.SectionIndex);
        HashValue(hash, key.ContentRevision);
        HashValue(hash, key.VertexStride);
        HashValue(hash, key.PositionOffset);
        HashValue(hash, key.VertexCount);
        HashValue(hash, key.IndexCount);
        HashValue(hash, static_cast<uint64_t>(key.AlphaMode));
        HashValue(hash, key.bOpaque ? 1u : 0u);
        HashValue(hash, key.bDoubleSided ? 1u : 0u);
        return static_cast<size_t>(hash);
    }

    size_t RayTracingInstanceIdentityHash::operator()(
        const RayTracingInstanceIdentity& identity) const noexcept
    {
        uint64_t hash = FnvOffset;
        HashValue(hash, identity.SceneGeneration);
        HashValue(hash, static_cast<uint64_t>(
            static_cast<int64_t>(identity.EntityId)));
        HashValue(hash, identity.ObjectOrdinal);
        HashValue(hash, identity.SectionIndex);
        HashString(hash, identity.MeshAssetPath);
        return static_cast<size_t>(hash);
    }

    uint32_t RayTracingStableInstanceRegistry::Resolve(
        const RayTracingInstanceIdentity& identity)
    {
        if (identity.SceneGeneration == 0)
        {
            throw std::invalid_argument(
                "Ray-tracing stable instance identity requires a non-zero scene generation");
        }
        if (mSceneGeneration != identity.SceneGeneration)
            Reset(identity.SceneGeneration);
        RayTracingInstanceIdentity normalized = identity;
        if (normalized.EntityId >= 0)
            normalized.ObjectOrdinal = 0;
        const auto existing = mIds.find(normalized);
        if (existing != mIds.end())
            return existing->second;
        if (mNextId >= (1u << 24u))
        {
            throw std::overflow_error(
                "Ray-tracing stable instance registry exhausted the 24-bit instance ID space");
        }
        const uint32_t id = mNextId++;
        mIds.emplace(std::move(normalized), id);
        return id;
    }

    void RayTracingStableInstanceRegistry::Reset(
        uint64_t sceneGeneration) noexcept
    {
        mIds.clear();
        mSceneGeneration = sceneGeneration;
        mNextId = 0;
    }

    uint64_t RayTracingStableInstanceRegistry::GetSceneGeneration() const noexcept
    {
        return mSceneGeneration;
    }

    size_t RayTracingStableInstanceRegistry::Size() const noexcept
    {
        return mIds.size();
    }
}
