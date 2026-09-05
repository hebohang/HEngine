#include "Hepch.h"

#include "Runtime/Graphics/RayTracing/RayTracingScene.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
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

        template<typename Value>
        void HashValue(uint64_t& hash, const Value& value) noexcept
        {
            HashBytes(hash, &value, sizeof(value));
        }

        [[nodiscard]] uint32_t RoundCapacity(uint32_t requested) noexcept
        {
            return std::max(std::bit_ceil(std::max(requested, 1u)), 16u);
        }

        [[nodiscard]] bool IsFinite(const glm::mat4& matrix) noexcept
        {
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                {
                    if (!std::isfinite(matrix[column][row]))
                        return false;
                }
            }
            return true;
        }

        void StoreNvrhiTransform(
            const glm::mat4& matrix,
            nvrhi::rt::AffineTransform& transform) noexcept
        {
            transform[0] = matrix[0][0];
            transform[1] = matrix[1][0];
            transform[2] = matrix[2][0];
            transform[3] = matrix[3][0];
            transform[4] = matrix[0][1];
            transform[5] = matrix[1][1];
            transform[6] = matrix[2][1];
            transform[7] = matrix[3][1];
            transform[8] = matrix[0][2];
            transform[9] = matrix[1][2];
            transform[10] = matrix[2][2];
            transform[11] = matrix[3][2];
        }

        [[nodiscard]] float Luminance(const glm::vec3& value) noexcept
        {
            return glm::dot(value, glm::vec3(0.2126f, 0.7152f, 0.0722f));
        }

        [[nodiscard]] RayTracingMaterialRecord MakeMaterialRecord(
            const RenderMeshSection::MaterialInstanceSnapshot& snapshot) noexcept
        {
            RayTracingMaterialRecord record;
            if (!snapshot.bValid)
                return record;
            record.BaseColorMetallic = glm::vec4(
                glm::max(glm::vec3(snapshot.BaseColor), glm::vec3(0.0f)),
                std::clamp(snapshot.Metallic, 0.0f, 1.0f));
            record.EmissiveRoughness = glm::vec4(
                glm::max(snapshot.Emissive, glm::vec3(0.0f)),
                std::clamp(snapshot.Roughness, 0.045f, 1.0f));
            return record;
        }

        [[nodiscard]] uint64_t MaterialFingerprint(
            const RenderMeshSection::MaterialInstanceSnapshot& snapshot) noexcept
        {
            uint64_t hash = FnvOffset;
            HashString(hash, snapshot.AssetPath);
            HashString(hash, snapshot.ShaderPath);
            HashValue(hash, snapshot.Revision);
            HashValue(hash, snapshot.BaseColor);
            HashValue(hash, snapshot.Emissive);
            HashValue(hash, snapshot.Metallic);
            HashValue(hash, snapshot.Roughness);
            HashValue(hash, snapshot.AlphaMode);
            HashValue(hash, snapshot.bDoubleSided);
            return hash == 0 ? 1 : hash;
        }

        struct GeometryBuildPassData
        {
            BufferHandle Vertices;
            BufferHandle Indices;
            AccelerationStructureHandle Blas;
        };

        struct TlasBuildPassData
        {
            std::vector<AccelerationStructureHandle> Blases;
            AccelerationStructureHandle Tlas;
        };

        struct SceneTableUploadPassData
        {
            BufferHandle Vertices;
            BufferHandle Indices;
            BufferHandle Geometries;
            BufferHandle Materials;
            BufferHandle Instances;
            BufferHandle EmissiveTriangles;
        };
    }

    struct RayTracingScene::Impl
    {
        struct GeometryEntry
        {
            RayTracingGeometryCacheKey Key;
            uint32_t SourceSection = 0;
            uint32_t VertexOffset = 0;
            uint32_t IndexOffset = 0;
            uint32_t VertexCount = 0;
            uint32_t IndexCount = 0;
            nvrhi::rt::GeometryDesc Geometry;
            nvrhi::rt::AccelStructHandle Blas;
            bool bBuilt = false;
            bool bNeedsBuild = true;
        };

        struct RetiredResources
        {
            uint64_t Submission = 0;
            RayTracingSceneGpuResources Scene;
            std::vector<nvrhi::rt::AccelStructHandle> Blases;
        };

        explicit Impl(const RayTracingSceneDesc& desc)
            : Device(desc.Device)
            , FramesInFlight(desc.FramesInFlight)
            , InitialInstanceCapacity(desc.InitialInstanceCapacity)
            , bCreateAccelerationStructures(desc.bCreateAccelerationStructures)
        {
            if (!Device)
                throw std::invalid_argument("RayTracingScene requires a valid NVRHI device");
            if (FramesInFlight == 0)
                throw std::invalid_argument("RayTracingScene requires at least one frame in flight");
            if (InitialInstanceCapacity == 0)
                throw std::invalid_argument("RayTracingScene initial instance capacity must be non-zero");
        }

        [[nodiscard]] uint32_t GetOrCreateStableInstanceId(
            const RayTracingInstanceIdentity& identity)
        {
            return StableInstanceRegistry.Resolve(identity);
        }

        void RetireCurrentResources()
        {
            if (!GpuResources.SceneTlas && !GpuResources.Vertices)
                return;
            RetiredResources retired;
            retired.Submission = LastSubmittedSubmission;
            retired.Scene = std::move(GpuResources);
            retired.Blases.reserve(Geometries.size());
            for (GeometryEntry& geometry : Geometries)
                retired.Blases.push_back(std::move(geometry.Blas));
            Retired.push_back(std::move(retired));
            GpuResources = {};
        }

        void CreateBuffer(
            nvrhi::BufferHandle& destination,
            uint64_t byteSize,
            uint32_t structStride,
            std::string debugName,
            bool bBuildInput,
            bool bRaw,
            bool bVertexBuffer = false,
            bool bIndexBuffer = false)
        {
            nvrhi::BufferDesc desc;
            desc.setByteSize(std::max<uint64_t>(byteSize, 16u))
                .setStructStride(structStride)
                .setCanHaveRawViews(bRaw)
                .setIsAccelStructBuildInput(bBuildInput)
                .setIsVertexBuffer(bVertexBuffer)
                .setIsIndexBuffer(bIndexBuffer)
                .setDebugName(debugName);
            destination = Device->createBuffer(desc);
            if (!destination)
                throw std::runtime_error("NVRHI failed to create " + debugName);
        }

        void RecreateTopologyResources()
        {
            RetireCurrentResources();

            const uint64_t vertexBytes =
                static_cast<uint64_t>(Vertices.size()) * sizeof(RayTracingVertex);
            const uint64_t indexBytes =
                static_cast<uint64_t>(Indices.size()) * sizeof(uint32_t);
            const uint64_t geometryBytes =
                static_cast<uint64_t>(GeometryRecords.size()) *
                sizeof(RayTracingGeometryRecord);
            const uint64_t materialBytes =
                static_cast<uint64_t>(MaterialCapacity) *
                sizeof(RayTracingMaterialRecord);
            const uint64_t instanceBytes =
                static_cast<uint64_t>(InstanceCapacity) *
                sizeof(RayTracingInstanceRecord);
            const uint64_t lightBytes =
                static_cast<uint64_t>(EmissiveTriangleCapacity) *
                sizeof(RayTracingEmissiveTriangle);

            CreateBuffer(
                GpuResources.Vertices,
                vertexBytes,
                sizeof(RayTracingVertex),
                "Hardware RT Scene Vertices",
                bCreateAccelerationStructures,
                true,
                true,
                false);
            CreateBuffer(
                GpuResources.Indices,
                indexBytes,
                0,
                "Hardware RT Scene Indices",
                bCreateAccelerationStructures,
                true,
                false,
                true);
            CreateBuffer(
                GpuResources.Geometries,
                geometryBytes,
                sizeof(RayTracingGeometryRecord),
                "Hardware RT Geometry Table",
                false,
                false);
            CreateBuffer(
                GpuResources.Materials,
                materialBytes,
                sizeof(RayTracingMaterialRecord),
                "Hardware RT Material Table",
                false,
                false);
            CreateBuffer(
                GpuResources.Instances,
                instanceBytes,
                sizeof(RayTracingInstanceRecord),
                "Hardware RT Instance Table",
                false,
                false);
            CreateBuffer(
                GpuResources.EmissiveTriangles,
                lightBytes,
                sizeof(RayTracingEmissiveTriangle),
                "Hardware RT Emissive Triangle Table",
                false,
                false);

            for (GeometryEntry& geometry : Geometries)
            {
                if (!bCreateAccelerationStructures)
                {
                    geometry.Geometry = {};
                    geometry.Blas = nullptr;
                    geometry.bBuilt = false;
                    geometry.bNeedsBuild = false;
                    continue;
                }
                nvrhi::rt::GeometryTriangles triangles;
                triangles.setVertexBuffer(GpuResources.Vertices)
                    .setVertexOffset(
                        static_cast<uint64_t>(geometry.VertexOffset) *
                        sizeof(RayTracingVertex))
                    .setVertexCount(geometry.VertexCount)
                    .setVertexStride(sizeof(RayTracingVertex))
                    .setVertexFormat(nvrhi::Format::RGB32_FLOAT)
                    .setIndexBuffer(GpuResources.Indices)
                    .setIndexOffset(
                        static_cast<uint64_t>(geometry.IndexOffset) *
                        sizeof(uint32_t))
                    .setIndexCount(geometry.IndexCount)
                    .setIndexFormat(nvrhi::Format::R32_UINT);
                geometry.Geometry = nvrhi::rt::GeometryDesc()
                    .setTriangles(triangles)
                    .setFlags(nvrhi::rt::GeometryFlags::Opaque);
                nvrhi::rt::AccelStructDesc blasDesc;
                blasDesc.addBottomLevelGeometry(geometry.Geometry)
                    .setBuildFlags(nvrhi::rt::AccelStructBuildFlags::PreferFastTrace)
                    .setDebugName(
                        "Hardware RT BLAS " + geometry.Key.MeshAssetPath + "#" +
                        std::to_string(geometry.Key.SectionIndex));
                geometry.Blas = Device->createAccelStruct(blasDesc);
                if (!geometry.Blas)
                    throw std::runtime_error("NVRHI failed to create a hardware RT BLAS");
                geometry.bBuilt = false;
                geometry.bNeedsBuild = true;
            }

            if (bCreateAccelerationStructures)
            {
                nvrhi::rt::AccelStructDesc tlasDesc;
                tlasDesc.setTopLevelMaxInstances(InstanceCapacity)
                    .setBuildFlags(
                        nvrhi::rt::AccelStructBuildFlags::AllowUpdate |
                        nvrhi::rt::AccelStructBuildFlags::PreferFastTrace)
                    .setDebugName("Hardware RT Scene TLAS");
                GpuResources.SceneTlas = Device->createAccelStruct(tlasDesc);
                if (!GpuResources.SceneTlas)
                {
                    throw std::runtime_error(
                        "NVRHI failed to create the hardware RT scene TLAS");
                }
            }

            GpuResources.Generation = ++ResourceGeneration;
            bSceneBuffersUploaded = false;
            bSceneBufferUploadRequired = true;
            bTlasBuilt = false;
            bTlasBuildRequired = bCreateAccelerationStructures;
            bGraphRebuildRequested = true;
            Stats.GeometryContentBytes = vertexBytes + indexBytes;
            Stats.SceneTableBytes =
                geometryBytes + materialBytes + instanceBytes + lightBytes;
            Stats.ActiveBufferAllocationRequirementBytes = 0;
            for (const nvrhi::BufferHandle& buffer : {
                GpuResources.Vertices,
                GpuResources.Indices,
                GpuResources.Geometries,
                GpuResources.Materials,
                GpuResources.Instances,
                GpuResources.EmissiveTriangles,
            })
            {
                Stats.ActiveBufferAllocationRequirementBytes +=
                    Device->getBufferMemoryRequirements(buffer).size;
            }
            Stats.ActiveAccelerationStructureAllocationRequirementBytes = 0;
            if (GpuResources.SceneTlas)
            {
                Stats.ActiveAccelerationStructureAllocationRequirementBytes +=
                    Device->getAccelStructMemoryRequirements(
                        GpuResources.SceneTlas).size;
                for (const GeometryEntry& geometry : Geometries)
                {
                    Stats.ActiveAccelerationStructureAllocationRequirementBytes +=
                        Device->getAccelStructMemoryRequirements(
                            geometry.Blas).size;
                }
            }
        }

        void BuildCpuScene(
            const RenderWorld& world,
            const RayTracingSceneRevisions& revisions)
        {
            const bool bTransformChanged = !bHasPreparedScene ||
                revisions.Transform != Revisions.Transform;
            const bool bSceneTablesUnchanged = bHasPreparedScene &&
                revisions.Scene == Revisions.Scene &&
                revisions.GeometryTopology == Revisions.GeometryTopology &&
                !bTransformChanged &&
                revisions.Material == Revisions.Material;
            if (bSceneTablesUnchanged)
            {
                // Point, directional and environment lights are consumed by
                // GI passes directly; they do not change these scene tables.
                Revisions.Light = revisions.Light;
                return;
            }

            std::vector<RayTracingGeometryCacheKey> nextGeometryKeys;
            RayTracingGeometryContentRevisionMap fallbackGeometryRevisions;
            struct Candidate
            {
                uint32_t ObjectIndex = 0;
                uint32_t SectionIndex = 0;
                const RenderObject* Object = nullptr;
                const RenderMeshSection* Section = nullptr;
                RayTracingGeometryCacheKey GeometryKey;
            };
            std::vector<Candidate> candidates;
            Stats.ExcludedInstanceCount = 0;
            Stats.ExcludedBySupport.fill(0u);
            Stats.InvalidTransformExcludedCount = 0u;

            for (uint32_t objectIndex = 0;
                objectIndex < static_cast<uint32_t>(world.Objects.size());
                ++objectIndex)
            {
                const RenderObject& object = world.Objects[objectIndex];
                if (!object.bVisible)
                    continue;
                for (uint32_t sectionIndex = 0;
                    sectionIndex < static_cast<uint32_t>(object.Sections.size());
                    ++sectionIndex)
                {
                    const RenderMeshSection& section = object.Sections[sectionIndex];
                    const RayTracingGeometrySupport support =
                        ClassifyRayTracingGeometry(object, section);
                    if (support !=
                        RayTracingGeometrySupport::SupportedStaticOpaque)
                    {
                        ++Stats.ExcludedInstanceCount;
                        ++Stats.ExcludedBySupport[static_cast<size_t>(support)];
                        continue;
                    }
                    if (!IsFinite(object.WorldTransform) ||
                        !IsFinite(object.PreviousWorldTransform))
                    {
                        ++Stats.ExcludedInstanceCount;
                        ++Stats.InvalidTransformExcludedCount;
                        continue;
                    }
                    const SubMesh& subMesh = *section.SubMeshResource;
                    const auto precomputedRevision =
                        revisions.GeometryContentRevisions.find(&subMesh);
                    const uint64_t contentRevision = precomputedRevision !=
                            revisions.GeometryContentRevisions.end()
                        ? precomputedRevision->second
                        : MemoizeRayTracingGeometryContentRevision(
                            fallbackGeometryRevisions,
                            subMesh);
                    RayTracingGeometryCacheKey key =
                        MakeRayTracingGeometryCacheKey(
                            object.MeshAssetPath,
                            sectionIndex,
                            subMesh,
                            contentRevision,
                            section.AlphaMode,
                            true,
                            section.bDoubleSided);
                    nextGeometryKeys.push_back(key);
                    candidates.push_back(Candidate{
                        .ObjectIndex = objectIndex,
                        .SectionIndex = sectionIndex,
                        .Object = &object,
                        .Section = &section,
                        .GeometryKey = std::move(key),
                    });
                }
            }

            const auto geometryKeyLess = [](
                const RayTracingGeometryCacheKey& left,
                const RayTracingGeometryCacheKey& right)
                {
                    if (left.MeshAssetPath != right.MeshAssetPath)
                        return left.MeshAssetPath < right.MeshAssetPath;
                    if (left.SectionIndex != right.SectionIndex)
                        return left.SectionIndex < right.SectionIndex;
                    if (left.ContentRevision != right.ContentRevision)
                        return left.ContentRevision < right.ContentRevision;
                    return left.VertexStride < right.VertexStride;
                };
            std::sort(
                nextGeometryKeys.begin(),
                nextGeometryKeys.end(),
                geometryKeyLess);
            nextGeometryKeys.erase(
                std::unique(nextGeometryKeys.begin(), nextGeometryKeys.end()),
                nextGeometryKeys.end());

            std::vector<RayTracingGeometryCacheKey> currentKeys;
            currentKeys.reserve(Geometries.size());
            for (const GeometryEntry& geometry : Geometries)
                currentKeys.push_back(geometry.Key);
            std::sort(currentKeys.begin(), currentKeys.end(), geometryKeyLess);
            const bool bTopologyChanged =
                revisions.Scene != Revisions.Scene ||
                revisions.GeometryTopology != Revisions.GeometryTopology ||
                currentKeys != nextGeometryKeys;

            if (revisions.Scene != Revisions.Scene)
            {
                StableInstanceRegistry.Reset(revisions.Scene);
            }

            if (bTopologyChanged)
            {
                // Retain the old BLAS handles before replacing cache entries;
                // submitted command lists may still reference them.
                RetireCurrentResources();
                Vertices.clear();
                Indices.clear();
                GeometryRecords.clear();
                Geometries.clear();
                GeometryLookup.clear();

                for (const Candidate& candidate : candidates)
                {
                    if (GeometryLookup.contains(candidate.GeometryKey))
                        continue;
                    const SubMesh& mesh = *candidate.Section->SubMeshResource;
                    GeometryEntry geometry;
                    geometry.Key = candidate.GeometryKey;
                    geometry.SourceSection = candidate.SectionIndex;
                    geometry.VertexOffset = static_cast<uint32_t>(Vertices.size());
                    geometry.IndexOffset = static_cast<uint32_t>(Indices.size());
                    geometry.VertexCount =
                        static_cast<uint32_t>(mesh.mStaticVertices.size());
                    geometry.IndexCount =
                        static_cast<uint32_t>(mesh.GetIndices().size());
                    for (const StaticVertex& vertex : mesh.mStaticVertices)
                    {
                        Vertices.push_back(RayTracingVertex{
                            .Position = glm::vec4(vertex.Pos, 1.0f),
                            .Normal = glm::vec4(vertex.Normal, 0.0f),
                        });
                    }
                    Indices.insert(
                        Indices.end(),
                        mesh.GetIndices().begin(),
                        mesh.GetIndices().end());
                    const uint32_t geometryIndex =
                        static_cast<uint32_t>(Geometries.size());
                    GeometryLookup.emplace(geometry.Key, geometryIndex);
                    GeometryRecords.push_back(RayTracingGeometryRecord{
                        .VertexIndexData = glm::uvec4(
                            geometry.VertexOffset,
                            geometry.IndexOffset,
                            geometry.VertexCount,
                            0u),
                        .GeometryData = glm::uvec4(
                            geometry.IndexCount,
                            geometry.SourceSection,
                            candidate.Section->bDoubleSided ? 1u : 0u,
                            0u),
                    });
                    Geometries.push_back(std::move(geometry));
                }
            }

            MaterialRecords.clear();
            InstanceRecords.clear();
            InstanceMappings.clear();
            TlasInstances.clear();
            EmissiveTriangles.clear();
            std::unordered_map<uint64_t, uint32_t> materialLookup;

            uint32_t highestStableInstanceId = 0;
            for (const Candidate& candidate : candidates)
            {
                const auto geometryIt = GeometryLookup.find(candidate.GeometryKey);
                if (geometryIt == GeometryLookup.end())
                    throw std::logic_error("RayTracingScene lost an active geometry key");
                const uint32_t geometryIndex = geometryIt->second;
                const uint64_t materialKey =
                    MaterialFingerprint(candidate.Section->MaterialInstance);
                uint32_t materialIndex = 0;
                const auto existingMaterial = materialLookup.find(materialKey);
                if (existingMaterial == materialLookup.end())
                {
                    materialIndex = static_cast<uint32_t>(MaterialRecords.size());
                    materialLookup.emplace(materialKey, materialIndex);
                    MaterialRecords.push_back(
                        MakeMaterialRecord(candidate.Section->MaterialInstance));
                }
                else
                {
                    materialIndex = existingMaterial->second;
                }

                const RayTracingInstanceIdentity identity{
                    .SceneGeneration = revisions.Scene,
                    .EntityId = candidate.Object->EntityId,
                    .ObjectOrdinal = candidate.Object->EntityId >= 0
                        ? 0u
                        : candidate.ObjectIndex,
                    .SectionIndex = candidate.SectionIndex,
                    .MeshAssetPath = candidate.Object->MeshAssetPath,
                };
                const uint32_t stableId = GetOrCreateStableInstanceId(identity);
                highestStableInstanceId = std::max(highestStableInstanceId, stableId);
                if (InstanceRecords.size() <= stableId)
                    InstanceRecords.resize(static_cast<size_t>(stableId) + 1u);

                glm::mat4 inverseTranspose(1.0f);
                const float determinant = glm::determinant(candidate.Object->WorldTransform);
                if (std::isfinite(determinant) && std::abs(determinant) > 1e-10f)
                {
                    inverseTranspose = glm::inverseTranspose(
                        candidate.Object->WorldTransform);
                }
                InstanceRecords[stableId] = RayTracingInstanceRecord{
                    .World = candidate.Object->WorldTransform,
                    .WorldInverseTranspose = inverseTranspose,
                    .PreviousWorld = candidate.Object->PreviousWorldTransform,
                    .Indices = glm::uvec4(
                        geometryIndex,
                        materialIndex,
                        candidate.ObjectIndex,
                        stableId),
                };

                RayTracingInstanceMapping mapping{
                    .StableInstanceId = stableId,
                    .ObjectIndex = candidate.ObjectIndex,
                    .GeometryIndex = geometryIndex,
                    .MaterialIndex = materialIndex,
                    .EntityId = candidate.Object->EntityId,
                    .SectionIndex = candidate.SectionIndex,
                    .MeshAssetPath = candidate.Object->MeshAssetPath,
                };
                InstanceMappings.push_back(std::move(mapping));

                nvrhi::rt::InstanceDesc instance;
                nvrhi::rt::AffineTransform transform;
                StoreNvrhiTransform(candidate.Object->WorldTransform, transform);
                instance.setTransform(transform)
                    .setInstanceID(stableId)
                    .setInstanceContributionToHitGroupIndex(0)
                    .setInstanceMask(0xff)
                    .setBLAS(Geometries[geometryIndex].Blas);
                nvrhi::rt::InstanceFlags instanceFlags =
                    nvrhi::rt::InstanceFlags::ForceOpaque;
                if (candidate.Section->bDoubleSided)
                    instanceFlags = instanceFlags |
                        nvrhi::rt::InstanceFlags::TriangleCullDisable;
                instance.setFlags(instanceFlags);
                TlasInstances.push_back(instance);

                const RayTracingMaterialRecord& material =
                    MaterialRecords[materialIndex];
                const glm::vec3 radiance(material.EmissiveRoughness);
                if (Luminance(radiance) <= 0.0f)
                    continue;
                const GeometryEntry& geometry = Geometries[geometryIndex];
                for (uint32_t index = 0; index + 2u < geometry.IndexCount; index += 3u)
                {
                    const uint32_t localIndex0 = Indices[geometry.IndexOffset + index];
                    const uint32_t localIndex1 = Indices[geometry.IndexOffset + index + 1u];
                    const uint32_t localIndex2 = Indices[geometry.IndexOffset + index + 2u];
                    if (localIndex0 >= geometry.VertexCount ||
                        localIndex1 >= geometry.VertexCount ||
                        localIndex2 >= geometry.VertexCount)
                    {
                        continue;
                    }
                    const glm::vec3 position0 = glm::vec3(
                        candidate.Object->WorldTransform *
                        Vertices[geometry.VertexOffset + localIndex0].Position);
                    const glm::vec3 position1 = glm::vec3(
                        candidate.Object->WorldTransform *
                        Vertices[geometry.VertexOffset + localIndex1].Position);
                    const glm::vec3 position2 = glm::vec3(
                        candidate.Object->WorldTransform *
                        Vertices[geometry.VertexOffset + localIndex2].Position);
                    const glm::vec3 crossValue =
                        glm::cross(position1 - position0, position2 - position0);
                    const float twiceArea = glm::length(crossValue);
                    if (!std::isfinite(twiceArea) || twiceArea <= 1e-10f)
                        continue;
                    EmissiveTriangles.push_back(RayTracingEmissiveTriangle{
                        .Position0 = glm::vec4(position0, 1.0f),
                        .Position1 = glm::vec4(position1, 1.0f),
                        .Position2 = glm::vec4(position2, 1.0f),
                        .NormalArea = glm::vec4(
                            crossValue / twiceArea,
                            0.5f * twiceArea),
                        .RadianceCdf = glm::vec4(radiance, 0.0f),
                        .Identity = glm::uvec4(
                            stableId,
                            geometryIndex,
                            index / 3u,
                            materialIndex),
                    });
                }
            }

            (void)BuildRayTracingEmissiveTriangleDistribution(EmissiveTriangles);

            const uint32_t requiredInstanceCapacity = RoundCapacity(std::max(
                std::max<uint32_t>(
                    static_cast<uint32_t>(TlasInstances.size()),
                    highestStableInstanceId + 1u),
                InitialInstanceCapacity));
            const uint32_t requiredMaterialCapacity = RoundCapacity(
                static_cast<uint32_t>(MaterialRecords.size()));
            const uint32_t requiredLightCapacity = RoundCapacity(
                static_cast<uint32_t>(EmissiveTriangles.size()));
            const bool bCapacityChanged =
                requiredInstanceCapacity != InstanceCapacity ||
                requiredMaterialCapacity != MaterialCapacity ||
                requiredLightCapacity != EmissiveTriangleCapacity;

            if (bTopologyChanged || bCapacityChanged || !GpuResources.SceneTlas)
            {
                InstanceCapacity = requiredInstanceCapacity;
                MaterialCapacity = requiredMaterialCapacity;
                EmissiveTriangleCapacity = requiredLightCapacity;
                RecreateTopologyResources();
                // BLAS handles are created above; rebuild TLAS instance pointers.
                for (size_t index = 0; index < candidates.size(); ++index)
                {
                    TlasInstances[index].setBLAS(
                        Geometries[InstanceMappings[index].GeometryIndex].Blas);
                }
            }
            else
            {
                for (GeometryEntry& geometry : Geometries)
                {
                    if (geometry.bBuilt)
                        ++Stats.BlasReuseCount;
                }
            }

            const bool bTlasTopologyCompatible =
                bTlasBuilt &&
                LastTlasInstanceCount == TlasInstances.size() &&
                revisions.GeometryTopology == Revisions.GeometryTopology &&
                revisions.Scene == Revisions.Scene;
            bCanUpdateTlas = bTlasTopologyCompatible;
            LastTlasInstanceCount = TlasInstances.size();
            Revisions.Scene = revisions.Scene;
            Revisions.GeometryTopology = revisions.GeometryTopology;
            Revisions.Transform = revisions.Transform;
            Revisions.Material = revisions.Material;
            Revisions.Light = revisions.Light;
            Revisions.GeometryContentRevisions.clear();
            bHasPreparedScene = true;
            bSceneBufferUploadRequired = true;
            if (bCreateAccelerationStructures &&
                (bTopologyChanged || bTransformChanged || !bTlasBuilt))
            {
                bTlasBuildRequired = true;
            }
            Stats.TlasInstanceCount = bCreateAccelerationStructures
                ? static_cast<uint32_t>(TlasInstances.size())
                : 0u;
            Stats.EmissiveTriangleCount =
                static_cast<uint32_t>(EmissiveTriangles.size());
        }

        [[nodiscard]] RenderGraphBufferDesc BufferGraphDesc(
            const nvrhi::BufferHandle& buffer) const
        {
            return {
                .DebugName = buffer->getDesc().debugName,
                .ByteSize = buffer->getDesc().byteSize,
                .StructStride = buffer->getDesc().structStride,
            };
        }

        nvrhi::DeviceHandle Device;
        uint32_t FramesInFlight = 0;
        uint32_t InitialInstanceCapacity = 0;
        bool bCreateAccelerationStructures = true;
        uint32_t InstanceCapacity = 0;
        uint32_t MaterialCapacity = 0;
        uint32_t EmissiveTriangleCapacity = 0;
        size_t LastTlasInstanceCount = 0;
        uint64_t LastSubmittedSubmission = 0;
        uint64_t ResourceGeneration = 0;
        RayTracingSceneRevisions Revisions{};
        RayTracingSceneGpuResources GpuResources;
        RayTracingSceneStats Stats;
        std::vector<RayTracingVertex> Vertices;
        std::vector<uint32_t> Indices;
        std::vector<RayTracingGeometryRecord> GeometryRecords;
        std::vector<RayTracingMaterialRecord> MaterialRecords;
        std::vector<RayTracingInstanceRecord> InstanceRecords;
        std::vector<RayTracingEmissiveTriangle> EmissiveTriangles;
        std::vector<nvrhi::rt::InstanceDesc> TlasInstances;
        std::vector<GeometryEntry> Geometries;
        std::unordered_map<
            RayTracingGeometryCacheKey,
            uint32_t,
            RayTracingGeometryCacheKeyHash> GeometryLookup;
        RayTracingStableInstanceRegistry StableInstanceRegistry;
        std::vector<RayTracingInstanceMapping> InstanceMappings;
        std::vector<RetiredResources> Retired;
        bool bSceneBuffersUploaded = false;
        bool bSceneBufferUploadRequired = true;
        bool bTlasBuilt = false;
        bool bCanUpdateTlas = false;
        bool bTlasBuildRequired = true;
        bool bHasPreparedScene = false;
        bool bGraphRebuildRequested = true;
        std::vector<uint32_t> BlasBuiltThisRecording;
        uint32_t BlasBuildCountThisRecording = 0;
        uint32_t TlasBuildCountThisRecording = 0;
        uint32_t TlasUpdateCountThisRecording = 0;
        bool bSceneBuffersUploadedBeforeRecording = false;
        bool bSceneBufferUploadRequiredBeforeRecording = true;
        bool bSceneBufferUploadRecorded = false;
        bool bTlasWasBuiltBeforeRecording = false;
        bool bTlasRecorded = false;
    };

    RayTracingScene::RayTracingScene(const RayTracingSceneDesc& desc)
        : mImpl(CreateScope<Impl>(desc))
    {
    }

    float BuildRayTracingEmissiveTriangleDistribution(
        std::span<RayTracingEmissiveTriangle> triangles) noexcept
    {
        float totalWeight = 0.0f;
        for (RayTracingEmissiveTriangle& triangle : triangles)
        {
            const float area = std::isfinite(triangle.NormalArea.w)
                ? std::max(triangle.NormalArea.w, 0.0f)
                : 0.0f;
            const float luminance = std::max(
                Luminance(glm::max(
                    glm::vec3(triangle.RadianceCdf),
                    glm::vec3(0.0f))),
                0.0f);
            const float weight = std::isfinite(luminance)
                ? area * luminance
                : 0.0f;
            totalWeight += weight;
            triangle.RadianceCdf.w = totalWeight;
        }
        if (totalWeight > 0.0f && std::isfinite(totalWeight))
        {
            for (RayTracingEmissiveTriangle& triangle : triangles)
                triangle.RadianceCdf.w /= totalWeight;
            triangles.back().RadianceCdf.w = 1.0f;
        }
        else
        {
            totalWeight = 0.0f;
            for (RayTracingEmissiveTriangle& triangle : triangles)
                triangle.RadianceCdf.w = 0.0f;
        }
        return totalWeight;
    }

    RayTracingScene::~RayTracingScene() = default;

    void RayTracingScene::Prepare(
        const RenderWorld& world,
        const RayTracingSceneRevisions& revisions,
        uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        mImpl->BuildCpuScene(world, revisions);
    }

    RayTracingSceneGraphResources RayTracingScene::AddBuildPasses(
        RenderGraph& renderGraph)
    {
        if (!mImpl->GpuResources.Vertices)
            throw std::logic_error("RayTracingScene AddBuildPasses requires Prepare");

        RayTracingSceneGraphResources resources;
        const nvrhi::ResourceStates inputInitialState =
            mImpl->bSceneBuffersUploaded
            ? nvrhi::ResourceStates::ShaderResource
            : nvrhi::ResourceStates::Common;
        resources.Vertices = renderGraph.ImportBuffer(
            mImpl->BufferGraphDesc(mImpl->GpuResources.Vertices),
            mImpl->GpuResources.Vertices,
            inputInitialState,
            nvrhi::ResourceStates::ShaderResource);
        resources.Indices = renderGraph.ImportBuffer(
            mImpl->BufferGraphDesc(mImpl->GpuResources.Indices),
            mImpl->GpuResources.Indices,
            inputInitialState,
            nvrhi::ResourceStates::ShaderResource);
        resources.Geometries = renderGraph.ImportBuffer(
            mImpl->BufferGraphDesc(mImpl->GpuResources.Geometries),
            mImpl->GpuResources.Geometries,
            inputInitialState,
            nvrhi::ResourceStates::ShaderResource);
        resources.Materials = renderGraph.ImportBuffer(
            mImpl->BufferGraphDesc(mImpl->GpuResources.Materials),
            mImpl->GpuResources.Materials,
            inputInitialState,
            nvrhi::ResourceStates::ShaderResource);
        resources.Instances = renderGraph.ImportBuffer(
            mImpl->BufferGraphDesc(mImpl->GpuResources.Instances),
            mImpl->GpuResources.Instances,
            inputInitialState,
            nvrhi::ResourceStates::ShaderResource);
        resources.EmissiveTriangles = renderGraph.ImportBuffer(
            mImpl->BufferGraphDesc(mImpl->GpuResources.EmissiveTriangles),
            mImpl->GpuResources.EmissiveTriangles,
            inputInitialState,
            nvrhi::ResourceStates::ShaderResource);

        renderGraph.AddPass<SceneTableUploadPassData>(
            mImpl->bCreateAccelerationStructures
                ? "HardwareRTSceneUpload"
                : "SceneSurfaceTableUpload",
            RenderGraphQueue::Graphics,
            [&resources](RenderGraphBuilder& builder, SceneTableUploadPassData& data)
            {
                data.Vertices = builder.CopyDest(resources.Vertices);
                data.Indices = builder.CopyDest(resources.Indices);
                data.Geometries = builder.CopyDest(resources.Geometries);
                data.Materials = builder.CopyDest(resources.Materials);
                data.Instances = builder.CopyDest(resources.Instances);
                data.EmissiveTriangles = builder.CopyDest(resources.EmissiveTriangles);
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const SceneTableUploadPassData& data)
            {
                if (!impl->bSceneBufferUploadRequired)
                    return;
                if (!impl->bSceneBufferUploadRecorded)
                {
                    impl->bSceneBuffersUploadedBeforeRecording =
                        impl->bSceneBuffersUploaded;
                    impl->bSceneBufferUploadRequiredBeforeRecording =
                        impl->bSceneBufferUploadRequired;
                    impl->bSceneBufferUploadRecorded = true;
                }
                (void)context.GetBuffer(data.Vertices, RenderGraphAccess::Write);
                (void)context.GetBuffer(data.Indices, RenderGraphAccess::Write);
                (void)context.GetBuffer(data.Geometries, RenderGraphAccess::Write);
                (void)context.GetBuffer(data.Materials, RenderGraphAccess::Write);
                (void)context.GetBuffer(data.Instances, RenderGraphAccess::Write);
                (void)context.GetBuffer(
                    data.EmissiveTriangles,
                    RenderGraphAccess::Write);
                nvrhi::ICommandList* commandList = context.GetCommandList();
                if (!impl->Vertices.empty())
                {
                    commandList->writeBuffer(
                        impl->GpuResources.Vertices,
                        impl->Vertices.data(),
                        impl->Vertices.size() * sizeof(RayTracingVertex));
                }
                if (!impl->Indices.empty())
                {
                    commandList->writeBuffer(
                        impl->GpuResources.Indices,
                        impl->Indices.data(),
                        impl->Indices.size() * sizeof(uint32_t));
                }
                if (!impl->GeometryRecords.empty())
                {
                    commandList->writeBuffer(
                        impl->GpuResources.Geometries,
                        impl->GeometryRecords.data(),
                        impl->GeometryRecords.size() *
                            sizeof(RayTracingGeometryRecord));
                }
                if (!impl->MaterialRecords.empty())
                {
                    commandList->writeBuffer(
                        impl->GpuResources.Materials,
                        impl->MaterialRecords.data(),
                        impl->MaterialRecords.size() *
                            sizeof(RayTracingMaterialRecord));
                }
                if (!impl->InstanceRecords.empty())
                {
                    commandList->writeBuffer(
                        impl->GpuResources.Instances,
                        impl->InstanceRecords.data(),
                        impl->InstanceRecords.size() *
                            sizeof(RayTracingInstanceRecord));
                }
                if (!impl->EmissiveTriangles.empty())
                {
                    commandList->writeBuffer(
                        impl->GpuResources.EmissiveTriangles,
                        impl->EmissiveTriangles.data(),
                        impl->EmissiveTriangles.size() *
                            sizeof(RayTracingEmissiveTriangle));
                }
                if (!impl->bSceneBuffersUploaded)
                {
                    impl->bSceneBuffersUploaded = true;
                    impl->bGraphRebuildRequested = true;
                }
                impl->bSceneBufferUploadRequired = false;
            });

        if (!mImpl->bCreateAccelerationStructures)
            return resources;

        std::vector<AccelerationStructureHandle> blasHandles;
        blasHandles.reserve(mImpl->Geometries.size());
        for (uint32_t geometryIndex = 0;
            geometryIndex < static_cast<uint32_t>(mImpl->Geometries.size());
            ++geometryIndex)
        {
            Impl::GeometryEntry& geometry = mImpl->Geometries[geometryIndex];
            const AccelerationStructureHandle blas =
                renderGraph.ImportAccelerationStructure(
                    RenderGraphAccelerationStructureDesc{
                        .DebugName = geometry.Blas->getDesc().debugName,
                    },
                    geometry.Blas,
                    geometry.bBuilt
                        ? nvrhi::ResourceStates::AccelStructRead
                        : nvrhi::ResourceStates::Common,
                    nvrhi::ResourceStates::AccelStructRead,
                    geometry.bBuilt);
            blasHandles.push_back(blas);
            if (!geometry.bNeedsBuild)
                continue;
            renderGraph.AddPass<GeometryBuildPassData>(
                "HardwareRTBLASBuild[" + std::to_string(geometryIndex) + "]",
                RenderGraphQueue::Graphics,
                [&resources, blas](
                    RenderGraphBuilder& builder,
                    GeometryBuildPassData& data)
                {
                    data.Vertices = builder.AccelerationStructureBuildInput(
                        resources.Vertices);
                    data.Indices = builder.AccelerationStructureBuildInput(
                        resources.Indices);
                    data.Blas = builder.AccelerationStructureBuildWrite(blas);
                },
                [impl = mImpl.get(), geometryIndex](
                    RenderGraphContext& context,
                    const GeometryBuildPassData& data)
                {
                    (void)context.GetBuffer(data.Vertices, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.Indices, RenderGraphAccess::Read);
                    nvrhi::rt::IAccelStruct* blas = context.GetAccelerationStructure(
                        data.Blas,
                        RenderGraphAccess::Write);
                    Impl::GeometryEntry& entry = impl->Geometries.at(geometryIndex);
                    if (!entry.bNeedsBuild)
                        return;
                    context.GetCommandList()->buildBottomLevelAccelStruct(
                        blas,
                        &entry.Geometry,
                        1,
                        nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
                    entry.bNeedsBuild = false;
                    entry.bBuilt = true;
                    impl->BlasBuiltThisRecording.push_back(geometryIndex);
                    ++impl->Stats.BlasBuildCount;
                    ++impl->BlasBuildCountThisRecording;
                    impl->bGraphRebuildRequested = true;
                });
        }

        resources.SceneTlas = renderGraph.ImportAccelerationStructure(
            RenderGraphAccelerationStructureDesc{
                .DebugName = mImpl->GpuResources.SceneTlas->getDesc().debugName,
            },
            mImpl->GpuResources.SceneTlas,
            mImpl->bTlasBuilt
                ? nvrhi::ResourceStates::AccelStructRead
                : nvrhi::ResourceStates::Common,
            nvrhi::ResourceStates::AccelStructRead,
            mImpl->bTlasBuilt);
        const bool bWillUpdateTlas =
            mImpl->bCanUpdateTlas && mImpl->bTlasBuilt;
        renderGraph.AddPass<TlasBuildPassData>(
            bWillUpdateTlas
                ? "HardwareRTTLASUpdate"
                : "HardwareRTTLASBuild",
            RenderGraphQueue::Graphics,
            [blasHandles, tlas = resources.SceneTlas](
                RenderGraphBuilder& builder,
                TlasBuildPassData& data)
            {
                data.Blases.reserve(blasHandles.size());
                for (AccelerationStructureHandle blas : blasHandles)
                    data.Blases.push_back(builder.AccelerationStructureRead(blas));
                data.Tlas = builder.AccelerationStructureBuildWrite(tlas);
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const TlasBuildPassData& data)
            {
                if (!impl->bTlasBuildRequired)
                    return;
                for (AccelerationStructureHandle blas : data.Blases)
                {
                    (void)context.GetAccelerationStructure(
                        blas,
                        RenderGraphAccess::Read);
                }
                nvrhi::rt::IAccelStruct* tlas = context.GetAccelerationStructure(
                    data.Tlas,
                    RenderGraphAccess::Write);
                const bool bUpdate = impl->bCanUpdateTlas && impl->bTlasBuilt;
                impl->bTlasWasBuiltBeforeRecording = impl->bTlasBuilt;
                context.GetCommandList()->buildTopLevelAccelStruct(
                    tlas,
                    impl->TlasInstances.data(),
                    impl->TlasInstances.size(),
                    bUpdate
                        ? nvrhi::rt::AccelStructBuildFlags::PerformUpdate
                        : nvrhi::rt::AccelStructBuildFlags::None);
                if (bUpdate)
                {
                    ++impl->Stats.TlasUpdateCount;
                    ++impl->TlasUpdateCountThisRecording;
                }
                else
                {
                    ++impl->Stats.TlasBuildCount;
                    ++impl->TlasBuildCountThisRecording;
                }
                if (!impl->bTlasBuilt)
                    impl->bGraphRebuildRequested = true;
                impl->bTlasBuilt = true;
                impl->bCanUpdateTlas = true;
                impl->bTlasBuildRequired = false;
                impl->bTlasRecorded = true;
            });
        return resources;
    }

    void RayTracingScene::UpdateImportedResources(
        RenderGraph& renderGraph,
        const RayTracingSceneGraphResources& resources) const
    {
        if (resources.SceneTlas && mImpl->GpuResources.SceneTlas)
        {
            renderGraph.SetAccelerationStructureResource(
                resources.SceneTlas,
                mImpl->GpuResources.SceneTlas);
        }
        renderGraph.SetBufferResource(resources.Vertices, mImpl->GpuResources.Vertices);
        renderGraph.SetBufferResource(resources.Indices, mImpl->GpuResources.Indices);
        renderGraph.SetBufferResource(
            resources.Geometries,
            mImpl->GpuResources.Geometries);
        renderGraph.SetBufferResource(resources.Materials, mImpl->GpuResources.Materials);
        renderGraph.SetBufferResource(resources.Instances, mImpl->GpuResources.Instances);
        renderGraph.SetBufferResource(
            resources.EmissiveTriangles,
            mImpl->GpuResources.EmissiveTriangles);
    }

    bool RayTracingScene::ConsumeGraphRebuildRequest() noexcept
    {
        return std::exchange(mImpl->bGraphRebuildRequested, false);
    }

    bool RayTracingScene::IsReady() const noexcept
    {
        return mImpl->GpuResources.Vertices && !mImpl->Geometries.empty() &&
            (!mImpl->bCreateAccelerationStructures ||
                mImpl->GpuResources.SceneTlas);
    }

    uint64_t RayTracingScene::GetResourceGeneration() const noexcept
    {
        return mImpl->ResourceGeneration;
    }

    const RayTracingSceneGpuResources& RayTracingScene::GetGpuResources() const noexcept
    {
        return mImpl->GpuResources;
    }

    const RayTracingSceneStats& RayTracingScene::GetStats() const noexcept
    {
        return mImpl->Stats;
    }

    std::span<const RayTracingInstanceMapping>
        RayTracingScene::GetInstanceMappings() const noexcept
    {
        return mImpl->InstanceMappings;
    }

    std::span<const RayTracingGeometryRecord>
        RayTracingScene::GetGeometryRecords() const noexcept
    {
        return mImpl->GeometryRecords;
    }

    const RayTracingInstanceMapping* RayTracingScene::FindInstanceMapping(
        uint32_t stableInstanceId) const noexcept
    {
        const auto mapping = std::ranges::find_if(
            mImpl->InstanceMappings,
            [stableInstanceId](const RayTracingInstanceMapping& candidate)
            {
                return candidate.StableInstanceId == stableInstanceId;
            });
        return mapping == mImpl->InstanceMappings.end() ? nullptr : &*mapping;
    }

    void RayTracingScene::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        mImpl->LastSubmittedSubmission = std::max(
            mImpl->LastSubmittedSubmission,
            submission);
        mImpl->BlasBuiltThisRecording.clear();
        mImpl->BlasBuildCountThisRecording = 0;
        mImpl->TlasBuildCountThisRecording = 0;
        mImpl->TlasUpdateCountThisRecording = 0;
        mImpl->bSceneBufferUploadRecorded = false;
        mImpl->bTlasRecorded = false;
    }

    void RayTracingScene::AbortFrame() noexcept
    {
        for (uint32_t geometryIndex : mImpl->BlasBuiltThisRecording)
        {
            if (geometryIndex >= mImpl->Geometries.size())
                continue;
            mImpl->Geometries[geometryIndex].bBuilt = false;
            mImpl->Geometries[geometryIndex].bNeedsBuild = true;
        }
        mImpl->BlasBuiltThisRecording.clear();
        mImpl->Stats.BlasBuildCount -= std::min(
            mImpl->Stats.BlasBuildCount,
            mImpl->BlasBuildCountThisRecording);
        mImpl->Stats.TlasBuildCount -= std::min(
            mImpl->Stats.TlasBuildCount,
            mImpl->TlasBuildCountThisRecording);
        mImpl->Stats.TlasUpdateCount -= std::min(
            mImpl->Stats.TlasUpdateCount,
            mImpl->TlasUpdateCountThisRecording);
        mImpl->BlasBuildCountThisRecording = 0;
        mImpl->TlasBuildCountThisRecording = 0;
        mImpl->TlasUpdateCountThisRecording = 0;
        if (mImpl->bSceneBufferUploadRecorded)
        {
            mImpl->bSceneBuffersUploaded =
                mImpl->bSceneBuffersUploadedBeforeRecording;
            mImpl->bSceneBufferUploadRequired =
                mImpl->bSceneBufferUploadRequiredBeforeRecording;
            mImpl->bSceneBufferUploadRecorded = false;
        }
        if (mImpl->bTlasRecorded)
        {
            mImpl->bTlasBuilt = mImpl->bTlasWasBuiltBeforeRecording;
            mImpl->bCanUpdateTlas = mImpl->bTlasBuilt;
            mImpl->bTlasBuildRequired = true;
            mImpl->bTlasRecorded = false;
        }
        mImpl->bGraphRebuildRequested = true;
    }

    void RayTracingScene::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->Retired,
            [completedSubmission](const Impl::RetiredResources& retired)
            {
                return retired.Submission <= completedSubmission;
            });
    }

    void RayTracingScene::Clear(uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        mImpl->RetireCurrentResources();
        mImpl->Vertices.clear();
        mImpl->Indices.clear();
        mImpl->GeometryRecords.clear();
        mImpl->MaterialRecords.clear();
        mImpl->InstanceRecords.clear();
        mImpl->EmissiveTriangles.clear();
        mImpl->TlasInstances.clear();
        mImpl->Geometries.clear();
        mImpl->GeometryLookup.clear();
        mImpl->StableInstanceRegistry.Reset(0);
        mImpl->InstanceMappings.clear();
        mImpl->Revisions = {};
        mImpl->bHasPreparedScene = false;
        mImpl->bSceneBuffersUploaded = false;
        mImpl->bSceneBufferUploadRequired = true;
        mImpl->bTlasBuilt = false;
        mImpl->bCanUpdateTlas = false;
        mImpl->bTlasBuildRequired = mImpl->bCreateAccelerationStructures;
        mImpl->Stats.ExcludedInstanceCount = 0;
        mImpl->Stats.ExcludedBySupport.fill(0u);
        mImpl->Stats.InvalidTransformExcludedCount = 0u;
        mImpl->Stats.ActiveBufferAllocationRequirementBytes = 0;
        mImpl->Stats.ActiveAccelerationStructureAllocationRequirementBytes = 0;
        mImpl->bGraphRebuildRequested = true;
        Collect(completedSubmission);
    }
}
