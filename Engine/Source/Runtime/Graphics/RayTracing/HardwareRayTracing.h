#pragma once

#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace HEngine
{
    struct HardwareRayTracingSupport
    {
        bool bAccelerationStructures = false;
        bool bPipelines = false;
        bool bRayQuery = false;
        bool bRealtimeRtgi = false;
        std::string Reason;
    };

    [[nodiscard]] HardwareRayTracingSupport QueryHardwareRayTracingSupport(
        const RenderCapabilities& capabilities);

    enum class RayTracingGeometrySupport : uint8_t
    {
        SupportedStaticOpaque,
        MissingGeometry,
        Skinned,
        SmoothShadingNormals,
        AlphaMask,
        AlphaBlend,
        CustomMaterialShader,
        TexturedMaterial,
        ProceduralGeometry,
        Vfx,
    };

    inline constexpr uint32_t c_RayTracingGeometrySupportCount =
        static_cast<uint32_t>(RayTracingGeometrySupport::Vfx) + 1u;

    [[nodiscard]] const char* ToString(RayTracingGeometrySupport support) noexcept;
    [[nodiscard]] RayTracingGeometrySupport ClassifyRayTracingGeometry(
        const RenderObject& object,
        const RenderMeshSection& section, bool bBasePbrMaterials = false) noexcept;

    struct RayTracingGeometryCacheKey
    {
        std::string MeshAssetPath;
        uint32_t SectionIndex = 0;
        uint64_t ContentRevision = 0;
        uint32_t VertexStride = 0;
        uint32_t PositionOffset = 0;
        uint32_t VertexCount = 0;
        uint32_t IndexCount = 0;
        RenderMaterialAlphaMode AlphaMode = RenderMaterialAlphaMode::Opaque;
        bool bOpaque = true;
        bool bDoubleSided = false;

        [[nodiscard]] bool operator==(const RayTracingGeometryCacheKey&) const = default;
    };

    struct RayTracingGeometryCacheKeyHash
    {
        [[nodiscard]] size_t operator()(
            const RayTracingGeometryCacheKey& key) const noexcept;
    };

    [[nodiscard]] uint64_t ComputeRayTracingGeometryContentRevision(
        const SubMesh& subMesh) noexcept;

    using RayTracingGeometryContentRevisionMap =
        std::unordered_map<const SubMesh*, uint64_t>;

    // Resolves a content revision at most once for each immutable SubMesh
    // snapshot referenced by the current RenderWorld frame.
    [[nodiscard]] uint64_t MemoizeRayTracingGeometryContentRevision(
        RayTracingGeometryContentRevisionMap& revisions,
        const SubMesh& subMesh) noexcept;

    [[nodiscard]] RayTracingGeometryCacheKey MakeRayTracingGeometryCacheKey(
        std::string_view meshAssetPath,
        uint32_t sectionIndex,
        const SubMesh& subMesh,
        RenderMaterialAlphaMode alphaMode,
        bool bOpaque,
        bool bDoubleSided = false);
    [[nodiscard]] RayTracingGeometryCacheKey MakeRayTracingGeometryCacheKey(
        std::string_view meshAssetPath,
        uint32_t sectionIndex,
        const SubMesh& subMesh,
        uint64_t contentRevision,
        RenderMaterialAlphaMode alphaMode,
        bool bOpaque,
        bool bDoubleSided = false);

    struct RayTracingInstanceIdentity
    {
        uint64_t SceneGeneration = 0;
        int EntityId = -1;
        uint32_t ObjectOrdinal = 0;
        uint32_t SectionIndex = 0;
        std::string MeshAssetPath;

        [[nodiscard]] bool operator==(const RayTracingInstanceIdentity&) const = default;
    };

    struct RayTracingInstanceIdentityHash
    {
        [[nodiscard]] size_t operator()(
            const RayTracingInstanceIdentity& identity) const noexcept;
    };

    class RayTracingStableInstanceRegistry final
    {
    public:
        [[nodiscard]] uint32_t Resolve(
            const RayTracingInstanceIdentity& identity);
        void Reset(uint64_t sceneGeneration) noexcept;
        [[nodiscard]] uint64_t GetSceneGeneration() const noexcept;
        [[nodiscard]] size_t Size() const noexcept;

    private:
        std::unordered_map<
            RayTracingInstanceIdentity,
            uint32_t,
            RayTracingInstanceIdentityHash> mIds;
        uint64_t mSceneGeneration = 0;
        uint32_t mNextId = 0;
    };
}
