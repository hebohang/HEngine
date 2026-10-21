#pragma once

#include "Runtime/Graphics/RayTracing/HardwareRayTracing.h"
#include "Runtime/Resource/ResType/TextureSampling.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace HEngine
{
    class TextureManager;
    inline constexpr uint32_t InvalidRayTracingSceneIndex =
        std::numeric_limits<uint32_t>::max();

    struct RayTracingSceneDesc
    {
        nvrhi::IDevice* Device = nullptr;
        uint32_t FramesInFlight = 1;
        uint32_t InitialInstanceCapacity = 64;
        // Surface-data-only scenes reuse the platform-neutral geometry tables
        // without creating BLAS/TLAS resources or invoking RT commands.
        bool bCreateAccelerationStructures = true;
        bool bBasePbrMaterials = false;
        TextureManager* Textures = nullptr;
    };

    struct RayTracingSceneRevisions
    {
        uint64_t Scene = 1;
        uint64_t GeometryTopology = 1;
        // Transform changes update instance/emissive tables and TLAS without
        // invalidating temporal GI history as a topology change would.
        uint64_t Transform = 1;
        uint64_t Material = 1;
        uint64_t Light = 1;
        // Ephemeral per-frame memo. SceneRenderer fills only admitted static
        // opaque SubMeshes; RayTracingScene consumes it during Prepare.
        RayTracingGeometryContentRevisionMap GeometryContentRevisions;
    };

    struct RayTracingVertex
    {
        glm::vec4 Position{ 0.0f, 0.0f, 0.0f, 1.0f };
        glm::vec4 Normal{ 0.0f, 1.0f, 0.0f, 0.0f };
        glm::vec4 TexCoord{ 0.0f };
        glm::vec4 Tangent{ 0.0f };
        glm::vec4 Bitangent{ 0.0f };
        glm::vec4 Color{ 1.0f };
    };

    struct RayTracingGeometryRecord
    {
        glm::uvec4 VertexIndexData{ 0u };
        // x: index count, y: source section, z: double-sided, w reserved.
        glm::uvec4 GeometryData{ 0u };
    };

    struct RayTracingMaterialRecord
    {
        // rgb: linear base color, w: metallic.
        glm::vec4 BaseColorMetallic{ 1.0f, 1.0f, 1.0f, 0.0f };
        // rgb: radiance, w: perceptual roughness.
        glm::vec4 EmissiveRoughness{ 0.0f, 0.0f, 0.0f, 1.0f };
        // Base color, normal, packed metallic/roughness, AO (AO is not transport).
        glm::uvec4 Textures0{ InvalidRayTracingSceneIndex };
        // Emission, separate metallic, separate roughness, normal encoding (1=BC5).
        glm::uvec4 Textures1{ InvalidRayTracingSceneIndex };
        // Normal strength, alpha factor, cutoff, alpha mode (1=mask).
        glm::vec4 Surface{ 1.0f, 1.0f, 0.5f, 0.0f };
        TextureSamplingGpu Sampling[7];
    };

    struct RayTracingAnalyticLight
    {
        glm::vec4 PositionType{ 0.0f }; // w: 0 directional, 1 point, 2 spot
        glm::vec4 DirectionRadius{ 0.0f };
        glm::vec4 Radiance{ 0.0f };
        glm::vec4 Cone{ 0.0f }; // inner/outer cosines
    };

    struct RayTracingInstanceRecord
    {
        glm::mat4 World{ 1.0f };
        glm::mat4 WorldInverseTranspose{ 1.0f };
        glm::mat4 PreviousWorld{ 1.0f };
        // x: geometry, y: material, z: object, w: stable instance id.
        glm::uvec4 Indices{ InvalidRayTracingSceneIndex };
    };

    struct RayTracingEmissiveTriangle
    {
        glm::vec4 Position0{ 0.0f };
        glm::vec4 Position1{ 0.0f };
        glm::vec4 Position2{ 0.0f };
        // xyz: one-sided normal, w: area in square metres.
        glm::vec4 NormalArea{ 0.0f };
        // rgb: emitted radiance, w: normalized cumulative probability.
        glm::vec4 RadianceCdf{ 0.0f };
        // x: stable instance id, y: geometry, z: primitive, w: material.
        glm::uvec4 Identity{ InvalidRayTracingSceneIndex };
    };

    static_assert(sizeof(RayTracingVertex) == 96);
    static_assert(offsetof(RayTracingVertex, Normal) == 16);
    static_assert(sizeof(RayTracingGeometryRecord) == 32);
    static_assert(offsetof(RayTracingGeometryRecord, GeometryData) == 16);
    static_assert(sizeof(RayTracingMaterialRecord) == 416);
    static_assert(offsetof(RayTracingMaterialRecord, EmissiveRoughness) == 16);
    static_assert(sizeof(RayTracingInstanceRecord) == 208);
    static_assert(offsetof(RayTracingInstanceRecord, Indices) == 192);
    static_assert(sizeof(RayTracingEmissiveTriangle) == 96);
    static_assert(offsetof(RayTracingEmissiveTriangle, Identity) == 80);

    // Builds an area*luminance importance distribution in-place and returns
    // its unnormalized total weight. A zero-weight table receives zero CDFs.
    [[nodiscard]] float BuildRayTracingEmissiveTriangleDistribution(
        std::span<RayTracingEmissiveTriangle> triangles) noexcept;

    struct RayTracingInstanceMapping
    {
        uint32_t StableInstanceId = InvalidRayTracingSceneIndex;
        uint32_t ObjectIndex = InvalidRayTracingSceneIndex;
        uint32_t GeometryIndex = InvalidRayTracingSceneIndex;
        uint32_t MaterialIndex = InvalidRayTracingSceneIndex;
        int EntityId = -1;
        uint32_t SectionIndex = 0;
        std::string MeshAssetPath;
    };

    struct RayTracingSceneStats
    {
        uint32_t BlasBuildCount = 0;
        uint32_t BlasReuseCount = 0;
        uint32_t TlasBuildCount = 0;
        uint32_t TlasUpdateCount = 0;
        uint32_t TlasInstanceCount = 0;
        uint32_t ExcludedInstanceCount = 0;
        std::array<uint32_t, c_RayTracingGeometrySupportCount>
            ExcludedBySupport{};
        uint32_t InvalidTransformExcludedCount = 0;
        uint32_t EmissiveTriangleCount = 0;
        uint64_t GeometryContentBytes = 0;
        uint64_t SceneTableBytes = 0;
        uint64_t ActiveBufferAllocationRequirementBytes = 0;
        uint64_t ActiveAccelerationStructureAllocationRequirementBytes = 0;
    };

    struct RayTracingSceneGraphResources
    {
        AccelerationStructureHandle SceneTlas;
        BufferHandle Vertices;
        BufferHandle Indices;
        BufferHandle Geometries;
        BufferHandle Materials;
        BufferHandle Instances;
        BufferHandle EmissiveTriangles;
        BufferHandle AnalyticLights;
    };

    struct RayTracingSceneGpuResources
    {
        nvrhi::rt::AccelStructHandle SceneTlas;
        nvrhi::BufferHandle Vertices;
        nvrhi::BufferHandle Indices;
        nvrhi::BufferHandle Geometries;
        nvrhi::BufferHandle Materials;
        nvrhi::BufferHandle Instances;
        nvrhi::BufferHandle EmissiveTriangles;
        nvrhi::BufferHandle AnalyticLights;
        uint64_t Generation = 0;
    };

    class RayTracingScene final
    {
    public:
        explicit RayTracingScene(const RayTracingSceneDesc& desc);
        ~RayTracingScene();

        RayTracingScene(const RayTracingScene&) = delete;
        RayTracingScene& operator=(const RayTracingScene&) = delete;

        // CPU scene extraction and resource preparation happen before graph
        // construction. Only static opaque built-in material geometry enters
        // the first vertical slice; exclusions are counted explicitly.
        void Prepare(
            const RenderWorld& world,
            const RayTracingSceneRevisions& revisions,
            uint64_t completedSubmission);

        [[nodiscard]] RayTracingSceneGraphResources AddBuildPasses(
            RenderGraph& renderGraph);
        void UpdateImportedResources(
            RenderGraph& renderGraph,
            const RayTracingSceneGraphResources& resources) const;

        [[nodiscard]] bool ConsumeGraphRebuildRequest() noexcept;
        [[nodiscard]] bool IsReady() const noexcept;
        [[nodiscard]] std::span<const nvrhi::TextureHandle> GetMaterialTextures() const noexcept;
        [[nodiscard]] uint64_t GetMaterialTextureGeneration() const noexcept;
        [[nodiscard]] bool UsesBasePbrMaterials() const noexcept;
        [[nodiscard]] uint64_t GetResourceGeneration() const noexcept;
        [[nodiscard]] const RayTracingSceneGpuResources& GetGpuResources() const noexcept;
        [[nodiscard]] const RayTracingSceneStats& GetStats() const noexcept;
        [[nodiscard]] std::span<const RayTracingInstanceMapping>
            GetInstanceMappings() const noexcept;
        [[nodiscard]] std::span<const RayTracingGeometryRecord>
            GetGeometryRecords() const noexcept;
        [[nodiscard]] const RayTracingInstanceMapping* FindInstanceMapping(
            uint32_t stableInstanceId) const noexcept;

        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Collect(uint64_t completedSubmission);
        void Clear(uint64_t completedSubmission);

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
