#include "Hepch.h"

#include "Runtime/UnitTest/MeshCpuDecodeTests.h"
#include "Runtime/Graphics/Mesh/MeshBindPose.h"
#include "Runtime/Graphics/Mesh/MeshLoader.h"
#include "Runtime/Graphics/Mesh/MeshResourceValidator.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Animation/FacialPerformance.h"
#include "_generated/serializer/FacialPerformance.gen.h"
#include "Runtime/Graphics/Mesh/MeshRuntimeResource.h"
#include "Runtime/Resource/ResType/Mesh.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "_generated/serializer/Mesh.gen.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        class TemporaryDirectory
        {
        public:
            TemporaryDirectory()
            {
                static std::atomic_uint64_t sCounter = 0;
                const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                mPath = std::filesystem::temp_directory_path() /
                    ("HEngineMeshDecodeTests-" + std::to_string(timestamp) + "-" +
                        std::to_string(sCounter.fetch_add(1, std::memory_order_relaxed)));
                std::filesystem::create_directories(mPath);
            }

            ~TemporaryDirectory()
            {
                std::error_code cleanupError;
                std::filesystem::remove_all(mPath, cleanupError);
            }

            [[nodiscard]] const std::filesystem::path& GetPath() const
            {
                return mPath;
            }

        private:
            std::filesystem::path mPath;
        };

        struct MeshFixture
        {
            MeshRes Resource;
            std::vector<std::uint8_t> BufferBytes;
            std::size_t StaticIndexOffset = 0;
        };

        template<typename T, std::size_t ValueCount>
        int AppendAccessor(
            MeshRes& resource,
            const std::array<T, ValueCount>& values,
            std::size_t elementCount,
            int componentType,
            std::string type)
        {
            MeshBuffer& buffer = resource.bufferVec.front();
            const std::size_t byteOffset = buffer.data.size();
            const std::size_t byteLength = sizeof(T) * ValueCount;
            buffer.data.resize(byteOffset + byteLength);
            std::memcpy(buffer.data.data() + byteOffset, values.data(), byteLength);

            const int bufferViewIndex = static_cast<int>(resource.bufferViewVec.size());
            MeshBufferView bufferView;
            bufferView.bufferIndex = 0;
            bufferView.byteOffset = byteOffset;
            bufferView.byteLength = byteLength;
            resource.bufferViewVec.push_back(std::move(bufferView));

            Accessor accessor;
            accessor.bufferViewIndex = bufferViewIndex;
            accessor.componentType = componentType;
            accessor.count = elementCount;
            accessor.type = std::move(type);
            resource.accessorVec.push_back(std::move(accessor));
            return static_cast<int>(resource.accessorVec.size() - 1);
        }

        MeshFixture MakeMeshFixture()
        {
            const std::array<float, 9> staticPositions = {
                0.0f, 0.0f, 0.0f,
                1.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f
            };
            const std::array<float, 9> staticNormals = {
                0.0f, 0.0f, 1.0f,
                0.0f, 0.0f, 1.0f,
                0.0f, 0.0f, 1.0f
            };
            const std::array<float, 9> skinnedPositions = {
                0.0f, 0.0f, 1.0f,
                1.0f, 0.0f, 1.0f,
                0.0f, 1.0f, 1.0f
            };
            const std::array<float, 9> skinnedNormals = staticNormals;
            const std::array<float, 12> joints = {
                1.0f, 2.0f, 3.0f, 4.0f,
                4.0f, 3.0f, 2.0f, 1.0f,
                2.0f, 0.0f, 0.0f, 0.0f
            };
            const std::array<float, 12> weights = {
                0.4f, 0.3f, 0.2f, 0.1f,
                1.0f, 0.0f, 0.0f, 0.0f,
                0.75f, 0.25f, 0.0f, 0.0f
            };
            const std::array<std::uint16_t, 3> staticIndices = { 0, 2, 1 };
            const std::array<std::uint8_t, 3> skinnedIndices = { 2, 1, 0 };

            MeshFixture fixture;
            MeshRes& resource = fixture.Resource;
            resource.name = "CpuDecodeFixture";
            resource.skeletonPath = "Assets\\Rig.hskel";
            resource.bufferVec.resize(1);
            resource.bufferVec.front().name = "CpuDecodeFixture";
            resource.bufferVec.front().uri = "Assets/Models/CpuDecodeFixture.bin";

            const int staticPositionAccessor = AppendAccessor(
                resource,
                staticPositions,
                3,
                HENGINE_COMPONENT_TYPE_FLOAT,
                "VEC3");
            const int staticNormalAccessor = AppendAccessor(
                resource,
                staticNormals,
                3,
                HENGINE_COMPONENT_TYPE_FLOAT,
                "VEC3");
            const int skinnedPositionAccessor = AppendAccessor(
                resource,
                skinnedPositions,
                3,
                HENGINE_COMPONENT_TYPE_FLOAT,
                "VEC3");
            const int skinnedNormalAccessor = AppendAccessor(
                resource,
                skinnedNormals,
                3,
                HENGINE_COMPONENT_TYPE_FLOAT,
                "VEC3");
            const int jointAccessor = AppendAccessor(
                resource,
                joints,
                3,
                HENGINE_COMPONENT_TYPE_FLOAT,
                "VEC4");
            const int weightAccessor = AppendAccessor(
                resource,
                weights,
                3,
                HENGINE_COMPONENT_TYPE_FLOAT,
                "VEC4");
            const int staticIndexAccessor = AppendAccessor(
                resource,
                staticIndices,
                3,
                HENGINE_COMPONENT_TYPE_UNSIGNED_SHORT,
                "SCALAR");
            const int skinnedIndexAccessor = AppendAccessor(
                resource,
                skinnedIndices,
                3,
                HENGINE_COMPONENT_TYPE_UNSIGNED_BYTE,
                "SCALAR");

            Primitive staticShared;
            staticShared.attributes["POSITION"] = staticPositionAccessor;
            staticShared.attributes["NORMAL"] = staticNormalAccessor;
            staticShared.indices = staticIndexAccessor;
            staticShared.materialResPath = "Assets\\Materials\\Shared.hmat";

            Primitive skinnedShared;
            skinnedShared.attributes["POSITION"] = skinnedPositionAccessor;
            skinnedShared.attributes["NORMAL"] = skinnedNormalAccessor;
            skinnedShared.attributes["JOINTS_0"] = jointAccessor;
            skinnedShared.attributes["WEIGHTS_0"] = weightAccessor;
            skinnedShared.indices = skinnedIndexAccessor;
            skinnedShared.materialResPath = "Assets/Materials/Shared.hmat";

            Primitive staticUnique = staticShared;
            staticUnique.materialResPath = "Assets/Materials/Unique.hmat";
            resource.primitives = {
                std::move(staticShared),
                std::move(skinnedShared),
                std::move(staticUnique)
            };

            resource.bufferVec.front().byteLength = static_cast<int>(resource.bufferVec.front().data.size());
            fixture.StaticIndexOffset = resource.bufferViewVec[
                resource.accessorVec[staticIndexAccessor].bufferViewIndex].byteOffset;
            fixture.BufferBytes = resource.bufferVec.front().data;
            resource.bufferVec.front().data.clear();
            return fixture;
        }

        void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            Require(file.is_open(), "Failed to create a mesh binary fixture.");
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.close();
            Require(!file.fail(), "Failed to write a mesh binary fixture.");
        }

        void WriteMeshResource(const std::filesystem::path& path, const MeshRes& resource)
        {
            Json json;
            Serializer::Serialize(json, resource);
            Utils::SaveJsonToFile(json, path);
        }

        void RequireFailure(
            const MeshDecodeResult& result,
            MeshDecodeError expectedCode,
            std::string_view message)
        {
            Require(!result && result.Code == expectedCode, message);
            Require(!result.Message.empty(), "A failed mesh decode did not provide a diagnostic message.");
            Require(result.Model.SkeletonPath.empty(), "A failed mesh decode published a partial skeleton path.");
            Require(result.Model.SubMeshes.empty(), "A failed mesh decode published partial submeshes.");
            Require(result.Model.MaterialPaths.empty(), "A failed mesh decode published partial materials.");
        }

        void VerifyValidDecode(
            const std::filesystem::path& meshPath,
            const std::filesystem::path& projectRoot)
        {
            MeshDecodeResult result = MeshLoader::DecodeModelHMESH(meshPath, projectRoot);
            if (!result)
            {
                throw std::runtime_error(
                    "A valid CPU mesh decode failed: " + result.Message);
            }

            Require(result.Model.SkeletonPath == "Assets/Rig.hskel",
                "The decoded skeleton path was not normalized.");
            Require(result.Model.MaterialPaths.size() == 2,
                "Decoded material paths were not deduplicated.");
            Require(result.Model.MaterialPaths[0] == "Assets/Materials/Shared.hmat" &&
                result.Model.MaterialPaths[1] == "Assets/Materials/Unique.hmat",
                "Decoded material path ordering is unstable.");
            Require(result.Model.SubMeshes.size() == 3,
                "The decoded model did not preserve all primitives.");

            const DecodedSubMesh& staticShared = result.Model.SubMeshes[0];
            Require(!staticShared.IsSkinned() && staticShared.StaticVertices.size() == 3 &&
                staticShared.SkinnedVertices.empty(),
                "A static primitive decoded with the wrong vertex payload.");
            Require(staticShared.Indices == std::vector<std::uint32_t>({ 0, 2, 1 }),
                "A uint16 index stream was not widened without reordering.");
            Require(staticShared.MaterialIndex == 0,
                "The first shared material mapped to the wrong index.");
            Require(staticShared.StaticVertices[1].Pos.x == 1.0f &&
                staticShared.StaticVertices[1].Pos.y == 0.0f &&
                staticShared.StaticVertices[1].Pos.z == 0.0f,
                "Static vertex positions changed during CPU decode.");
            Require(std::isfinite(staticShared.StaticVertices[0].Tangent.x) &&
                std::isfinite(staticShared.StaticVertices[0].Bitangent.y),
                "Generated static tangent data contains a non-finite value.");

            const DecodedSubMesh& skinnedShared = result.Model.SubMeshes[1];
            Require(skinnedShared.IsSkinned() && skinnedShared.StaticVertices.empty() &&
                skinnedShared.SkinnedVertices.size() == 3,
                "A skinned primitive decoded with the wrong vertex payload.");
            Require(skinnedShared.Indices == std::vector<std::uint32_t>({ 2, 1, 0 }),
                "A uint8 index stream was not widened without reordering.");
            Require(skinnedShared.MaterialIndex == 0,
                "A repeated material path was not mapped to the existing index.");
            Require(skinnedShared.SkinnedVertices[0].mBoneIDs[0] == 1 &&
                skinnedShared.SkinnedVertices[0].mBoneIDs[3] == 4 &&
                std::abs(skinnedShared.SkinnedVertices[0].mWeights[0] - 0.4f) < 1e-6f &&
                std::abs(skinnedShared.SkinnedVertices[0].mWeights[3] - 0.1f) < 1e-6f,
                "Skin joint or weight data changed during CPU decode.");
            for (int slot = 4; slot < MAX_BONE_INFLUENCE; ++slot)
                Require(skinnedShared.SkinnedVertices[0].mBoneIDs[slot] == -1 && skinnedShared.SkinnedVertices[0].mWeights[slot] == 0,
                    "Legacy four-influence decode left new slots active");

            const DecodedSubMesh& staticUnique = result.Model.SubMeshes[2];
            Require(!staticUnique.IsSkinned() && staticUnique.MaterialIndex == 1,
                "A unique material path mapped to the wrong index.");
        }

        void VerifyFailurePaths(
            const std::filesystem::path& projectRoot,
            const std::filesystem::path& validMeshPath,
            const MeshFixture& fixture)
        {
            std::stop_source cancelledSource;
            cancelledSource.request_stop();
            RequireFailure(
                MeshLoader::DecodeModelHMESH(validMeshPath, projectRoot, cancelledSource.get_token()),
                MeshDecodeError::Cancelled,
                "A pre-cancelled CPU mesh decode did not report cancellation.");

            RequireFailure(
                MeshLoader::DecodeModelHMESH(projectRoot / "Assets/Models/Missing.hmesh", projectRoot),
                MeshDecodeError::MetadataFileError,
                "A missing mesh metadata file returned the wrong error.");

            MeshRes unsafeResource = fixture.Resource;
            unsafeResource.bufferVec.front().uri = "../Outside.bin";
            const std::filesystem::path unsafeMeshPath = projectRoot / "Assets/Models/Unsafe.hmesh";
            WriteMeshResource(unsafeMeshPath, unsafeResource);
            RequireFailure(
                MeshLoader::DecodeModelHMESH(unsafeMeshPath, projectRoot),
                MeshDecodeError::InvalidResource,
                "A traversing mesh buffer URI was accepted by CPU decode.");

            MeshRes invalidDataResource = fixture.Resource;
            invalidDataResource.bufferVec.front().uri = "Assets/Models/InvalidData.bin";
            std::vector<std::uint8_t> invalidBytes = fixture.BufferBytes;
            const std::uint16_t invalidIndex = 9;
            std::memcpy(
                invalidBytes.data() + fixture.StaticIndexOffset,
                &invalidIndex,
                sizeof(invalidIndex));
            WriteBytes(projectRoot / "Assets/Models/InvalidData.bin", invalidBytes);
            const std::filesystem::path invalidDataMeshPath =
                projectRoot / "Assets/Models/InvalidData.hmesh";
            WriteMeshResource(invalidDataMeshPath, invalidDataResource);
            RequireFailure(
                MeshLoader::DecodeModelHMESH(invalidDataMeshPath, projectRoot),
                MeshDecodeError::InvalidResource,
                "An out-of-range mesh index was accepted by CPU decode.");
        }

        void VerifyMorphContract(const std::filesystem::path& projectRoot, const MeshFixture& fixture)
        {
            MeshRes resource = fixture.Resource;
            auto& primitive = resource.primitives[1];
            primitive.targets = { { { "POSITION", primitive.attributes.at("NORMAL") } } };
            primitive.targetNames = { "JawOpen" };
            primitive.targetWeights = { 0.0f };
            const auto path = projectRoot / "Assets/Models/Morph.hmesh";
            WriteMeshResource(path, resource);
            auto result = MeshLoader::DecodeModelHMESH(path, projectRoot);
            Require(static_cast<bool>(result), "Valid morph target failed decode");
            auto& decoded = result.Model.SubMeshes[1];
            Require(decoded.MorphTargets.size() == 1 && decoded.MorphTargets[0].Name == "JawOpen",
                "Native morph name or stream was lost");
            auto section = CreateRef<SubMesh>(std::move(decoded.SkinnedVertices), std::move(decoded.Indices));
            section->SetMorphTargets(std::move(decoded.MorphTargets));
            const float baseZ = section->mSkinnedVertices[0].Pos.z;
            Mesh first;
            first.mSubMeshes.push_back(section);
            Mesh second = first;
            first.UpdateMorphWeights({ { "JawOpen", 0.5f } });
            Require(std::abs(first.mSubMeshes[0]->mSkinnedVertices[0].Pos.z - baseZ - 0.5f) < 1e-6f,
                "Named morph did not deform the live vertex stream");
            Require(second.mSubMeshes[0]->mSkinnedVertices[0].Pos.z == baseZ,
                "Morph changed another mesh instance's shared prototype");
            const auto& vertex = first.mSubMeshes[0]->mSkinnedVertices[0];
            Require(vertex.PreviousPosition.z == baseZ, "Morph did not preserve prior-frame position");
            first.UpdateMorphWeights({ { "JawOpen", 0.0f } }, true);
            Require(vertex.Pos.z == baseZ && vertex.PreviousPosition.z == baseZ,
                "Morph reset accumulated offsets or preserved invalid history");
            auto& cached = *first.mSubMeshes[0];
            cached.UpdatePureMorphWeights({{"JawOpen", .5f}});
            const auto changedRevision = cached.GetDeformationRevision();
            Require(vertex.PreviousPosition.z == baseZ && std::abs(vertex.Pos.z - baseZ - .5f) < 1e-6f,
                "Cached morph changed-frame history did not retain its previous position");
            cached.UpdatePureMorphWeights({{"JawOpen", .5f}, {"UnusedTarget", 1}});
            Require(cached.GetDeformationRevision() == changedRevision + 1 && vertex.PreviousPosition == vertex.Pos,
                "First stable morph frame failed to settle its motion history");
            cached.UpdatePureMorphWeights({{"JawOpen", .5f}});
            cached.UpdatePureMorphWeights({{"JawOpen", .5f}}, true);
            Require(cached.GetDeformationRevision() == changedRevision + 1,
                "Settled morph published redundant vertex updates");
            auto cloned = CreateRef<SubMesh>(cached);
            cloned->UpdatePureMorphWeights({{"JawOpen", .5f}});
            Require(cloned->GetDeformationRevision() == changedRevision + 2,
                "A copied section reused another instance's morph cache identity");
            cloned->UpdatePureMorphWeights({{"JawOpen", 1}}, true);
            Require(std::abs(vertex.Pos.z - baseZ - .5f) < 1e-6f &&
                cloned->mSkinnedVertices[0].PreviousPosition == cloned->mSkinnedVertices[0].Pos,
                "Cached morph clone changed its source or lost reset history");
            const float adjacentWeight = std::nextafter(.5f, 1.0f);
            cached.UpdatePureMorphWeights({{"JawOpen", adjacentWeight}});
            Require(cached.GetDeformationRevision() == changedRevision + 2,
                "Cache approximated a distinct effective morph weight");
            cached.UpdatePureMorphWeights({{"JawOpen", 2}}, true);
            const auto clampedRevision = cached.GetDeformationRevision();
            cached.UpdatePureMorphWeights({{"JawOpen", 100}}, true);
            Require(cached.GetDeformationRevision() == clampedRevision,
                "Equivalent clamped weights bypassed the cache");
            cached.UpdatePureMorphWeights({{"JawOpen", std::numeric_limits<float>::quiet_NaN()}}, true);
            const auto neutralRevision = cached.GetDeformationRevision();
            cached.UpdatePureMorphWeights({});
            Require(cached.GetDeformationRevision() == neutralRevision && vertex.Pos.z == baseZ,
                "NaN-to-zero or authored default effective weights were cached incorrectly");
            auto replacementTargets = cached.GetMorphTargets();
            replacementTargets[0].DefaultWeight = .25f;
            cached.SetMorphTargets(std::move(replacementTargets));
            cached.UpdatePureMorphWeights({}, true);
            Require(cached.GetDeformationRevision() == neutralRevision + 1 &&
                std::abs(vertex.Pos.z - baseZ - .25f) < 1e-6f,
                "Target replacement/reimport retained a stale morph cache");
            StaticVertex staticVertex;
            staticVertex.Normal = glm::vec3(0, 0, 1);
            staticVertex.Tangent = glm::vec3(1, 0, 0);
            staticVertex.Bitangent = glm::vec3(0, 1, 0);
            SubMesh staticMorph(std::vector<StaticVertex>{staticVertex}, std::vector<uint32_t>{0});
            MorphTarget staticTarget;
            staticTarget.Name = "Lift"; staticTarget.Positions = {glm::vec3(0, 1, 0)};
            staticMorph.SetMorphTargets({staticTarget});
            staticMorph.UpdatePureMorphWeights({{"Lift", 1}});
            staticMorph.UpdatePureMorphWeights({{"Lift", 1}});
            const auto staticRevision = staticMorph.GetDeformationRevision();
            staticMorph.UpdatePureMorphWeights({{"Lift", 1}});
            Require(staticMorph.GetDeformationRevision() == staticRevision &&
                staticMorph.mStaticVertices[0].PreviousPosition == staticMorph.mStaticVertices[0].Pos &&
                staticMorph.mStaticVertices[0].Pos.y == 1,
                "Static morph history failed to settle or reused stale vertex data");
            MorphTarget zeroTarget;
            zeroTarget.Name = "ExportedButEmpty";
            zeroTarget.Positions = {glm::vec3(0)};
            SubMesh emptyMorph(std::vector<StaticVertex>{staticVertex}, std::vector<uint32_t>{0});
            emptyMorph.SetMorphTargets({zeroTarget});
            Require(emptyMorph.GetMorphTargets().size() == 1 && !emptyMorph.HasEffectiveMorphTargets(),
                "Zero-delta authored morph name was lost or marked deforming");
            emptyMorph.UpdatePureMorphWeights({{"ExportedButEmpty", 1}});
            Require(emptyMorph.GetDeformationRevision() == 0 &&
                emptyMorph.mStaticVertices[0].Pos == staticVertex.Pos,
                "Zero-delta morph caused a full vertex update");
            SubMesh mixedMorph(std::vector<StaticVertex>{staticVertex}, std::vector<uint32_t>{0});
            mixedMorph.SetMorphTargets({staticTarget, zeroTarget});
            mixedMorph.UpdatePureMorphWeights({{"Lift", 1}}, true);
            const auto mixedRevision = mixedMorph.GetDeformationRevision();
            mixedMorph.UpdatePureMorphWeights({{"Lift", 1}, {"ExportedButEmpty", 1}}, true);
            Require(mixedMorph.GetDeformationRevision() == mixedRevision &&
                mixedMorph.mStaticVertices[0].Pos.y == 1,
                "Changing only a zero-delta target invalidated the morph cache");
            primitive.targets[0]["POSITION"] = -1;
            WriteMeshResource(path, resource);
            RequireFailure(MeshLoader::DecodeModelHMESH(path, projectRoot), MeshDecodeError::InvalidResource,
                "Invalid morph accessor was accepted");
        }

        void VerifyFacialCurves()
        {
            FacialPerformanceRes performance;
            performance.Duration = 2.0f;
            FacialCurve curve;
            curve.Target = "JawOpen";
            FacialKey first;
            FacialKey middle;
            middle.Time = 1.0f;
            middle.Value = 1.0f;
            FacialKey last;
            last.Time = 2.0f;
            curve.Keys = { first, middle, last };
            performance.Curves.push_back(curve);
            FacialPerformance::Validate(performance);
            std::map<std::string, float> weights;
            FacialPerformance::Evaluate(performance, 0.5f, weights);
            Require(std::abs(weights.at("JawOpen") - 0.5f) < 1e-6f, "Facial interpolation failed");
            performance.Loop = true;
            FacialPerformance::Evaluate(performance, 2.5f, weights);
            Require(std::abs(weights.at("JawOpen") - 0.5f) < 1e-6f, "Facial loop seek failed");
            Json json;
            Serializer::Serialize(json, performance);
            FacialPerformanceRes restored;
            Serializer::Deserialize(json, restored);
            Require(restored.Curves.size() == 1 && restored.Curves[0].Target == "JawOpen" &&
                restored.Curves[0].Keys.size() == 3, "Facial curves did not survive reflected serialization");
            Require(FacialPerformance::Blink(0.063f, 3.0f) > 0.999f &&
                FacialPerformance::Blink(0.5f, 3.0f) == 0.0f && FacialPerformance::Blink(0.1f, 0.0f) == 0.0f,
                "Deterministic blink envelope failed");
            restored.Curves[0].Keys[1].Time = 0.0f;
            bool rejected = false;
            try { FacialPerformance::Validate(restored); } catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Unordered facial keys were accepted");
        }

		void VerifyExplicitMeshKeyContract(const std::filesystem::path& projectRoot)
		{
			const RuntimeResourceKey key = MeshRuntimeResource::MakeKey(
				projectRoot,
				"Assets\\Models//./CpuDecodeFixture.hmesh");
			Require(key.Kind == RuntimeResourceKind::Mesh &&
				key.Path == "Assets/Models/CpuDecodeFixture.hmesh" &&
				key.CachePath == "assets/models/cpudecodefixture.hmesh",
				"The explicit mesh resource root did not produce a canonical key.");

			bool bRejectedTraversal = false;
			try
			{
				(void)MeshRuntimeResource::MakeKey(projectRoot, "../Outside.hmesh");
			}
			catch (const std::invalid_argument&)
			{
				bRejectedTraversal = true;
			}
			Require(bRejectedTraversal,
				"The explicit mesh resource root accepted parent traversal.");
		}
    }

    void MeshCpuDecodeContract()
    {
        // No animation clip: bind skinning is hierarchy global * inverse bind,
        // not necessarily identity, and must retain joints above slot 99.
        SkeletonRes bindSkeleton;
        for (int index = 0; index < 137; ++index)
        {
            SkeletonBoneRes bone;
            bone.index = index;
            bone.name = "joint" + std::to_string(index);
            bone.parentIndex = index - 1;
            bone.localBindT = glm::vec3(0, .01f, 0);
            bindSkeleton.bones.push_back(bone);
            const glm::mat4 inverseBind = glm::translate(glm::mat4(1), glm::vec3(0, -.01f * (index + 1) + .25f, 0));
            bindSkeleton.inverseBindMatrices.emplace_back(glm::value_ptr(inverseBind), glm::value_ptr(inverseBind) + 16);
        }
        const auto bindPalette = BuildMeshBindPalette(bindSkeleton);
        Require(bindPalette.size() == 137 && std::abs(bindPalette[136][3].y - .25f) < 1e-5f,
            "Unanimated bind palette truncated >100 joints or assumed identity");
        std::swap(bindSkeleton.bones[0], bindSkeleton.bones[136]);
        std::swap(bindSkeleton.inverseBindMatrices[0], bindSkeleton.inverseBindMatrices[136]);
        Require(BuildMeshBindPalette(bindSkeleton) == bindPalette,
            "Bind hierarchy required parent-first storage instead of explicit indices");
        bindSkeleton.bones[136].parentIndex = 136;
        bool rejectedBindCycle = false;
        try { (void)BuildMeshBindPalette(bindSkeleton); }
        catch (const std::invalid_argument&) { rejectedBindCycle = true; }
        Require(rejectedBindCycle, "Cyclic bind skeleton was silently accepted");

        TemporaryDirectory temporaryDirectory;
        const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
        std::filesystem::create_directories(projectRoot / "Assets/Models");

        const MeshFixture fixture = MakeMeshFixture();
        const std::filesystem::path meshPath = projectRoot / "Assets/Models/CpuDecodeFixture.hmesh";
        WriteBytes(projectRoot / "Assets/Models/CpuDecodeFixture.bin", fixture.BufferBytes);
        WriteMeshResource(meshPath, fixture.Resource);

        VerifyValidDecode(meshPath, projectRoot);
        VerifyMorphContract(projectRoot, fixture);
        VerifyFacialCurves();
        VerifyFailurePaths(projectRoot, meshPath, fixture);
		VerifyExplicitMeshKeyContract(projectRoot);
        auto extended = fixture.Resource;
        extended.bufferVec[0].data = fixture.BufferBytes;
        // The legacy fixture ends in three uint8 indices; new float streams
        // require their own aligned offset without changing that index view.
        while (extended.bufferVec[0].data.size() % sizeof(float)) extended.bufferVec[0].data.push_back(0);
        for (int group = 1; group < 4; ++group)
        {
            std::array<float, 12> joints{}, weights{};
            for (int vertex = 0; vertex < 3; ++vertex)
                for (int lane = 0; lane < 4; ++lane)
                {
                    joints[vertex * 4 + lane] = static_cast<float>(group * 4 + lane + 100);
                    weights[vertex * 4 + lane] = 1;
                }
            extended.primitives[1].attributes["JOINTS_" + std::to_string(group)] =
                AppendAccessor(extended, joints, 3, HENGINE_COMPONENT_TYPE_FLOAT, "VEC4");
            extended.primitives[1].attributes["WEIGHTS_" + std::to_string(group)] =
                AppendAccessor(extended, weights, 3, HENGINE_COMPONENT_TYPE_FLOAT, "VEC4");
        }
        extended.bufferVec[0].byteLength = static_cast<int>(extended.bufferVec[0].data.size());
        const auto extendedValidation = MeshResourceValidator::ValidateLoaded(extended);
        Require(static_cast<bool>(extendedValidation), "Valid sixteen-influence resource rejected: " + extendedValidation.Message);
        auto missingPair = extended;
        missingPair.primitives[1].attributes.erase("WEIGHTS_2");
        Require(!MeshResourceValidator::ValidateLoaded(missingPair), "Unpaired additional skin group accepted");
        auto gap = extended;
        gap.primitives[1].attributes.erase("JOINTS_1"); gap.primitives[1].attributes.erase("WEIGHTS_1");
        Require(!MeshResourceValidator::ValidateLoaded(gap), "Non-contiguous skin groups accepted");
        auto tooMany = extended;
        tooMany.primitives[1].attributes["JOINTS_4"] = tooMany.primitives[1].attributes["JOINTS_3"];
        Require(!MeshResourceValidator::ValidateLoaded(tooMany), "Unsupported fifth skin group was silently ignored");
        auto wrongCount = extended;
        wrongCount.accessorVec[wrongCount.primitives[1].attributes["JOINTS_2"]].count = 2;
        Require(!MeshResourceValidator::ValidateLoaded(wrongCount), "Additional skin group vertex-count mismatch accepted");
        auto negativeJoint = extended;
        const auto& jointAccessor = negativeJoint.accessorVec[negativeJoint.primitives[1].attributes["JOINTS_3"]];
        const auto& jointView = negativeJoint.bufferViewVec[jointAccessor.bufferViewIndex];
        const float invalidJoint = -1;
        std::memcpy(negativeJoint.bufferVec[0].data.data() + jointView.byteOffset + jointAccessor.byteOffset, &invalidJoint, sizeof(float));
        Require(!MeshResourceValidator::ValidateLoaded(negativeJoint), "Negative native joint in additional group accepted");
        auto invalid = extended;
        const int extraWeights = invalid.primitives[1].attributes["WEIGHTS_3"];
        const auto& accessor = invalid.accessorVec[extraWeights];
        const auto& view = invalid.bufferViewVec[accessor.bufferViewIndex];
        const float invalidWeight = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(invalid.bufferVec[0].data.data() + view.byteOffset + accessor.byteOffset, &invalidWeight, sizeof(float));
        Require(!MeshResourceValidator::ValidateLoaded(invalid), "NaN in sixteenth-influence group accepted");
        WriteBytes(projectRoot / "Assets/Models/CpuDecodeFixture.bin", extended.bufferVec[0].data);
        extended.bufferVec[0].data.clear();
        WriteMeshResource(meshPath, extended);
        const auto full = MeshLoader::DecodeModelHMESH(meshPath, projectRoot);
        Require(static_cast<bool>(full), "Sixteen-influence native decode failed");
        const auto& vertex = full.Model.SubMeshes[1].SkinnedVertices[0];
        float total = 0;
        for (float weight : vertex.mWeights) total += weight;
        Require(std::abs(total - 1) < 1e-6f && vertex.mBoneIDs[4] == 104 && vertex.mBoneIDs[8] == 108 &&
            vertex.mBoneIDs[15] == 115 && vertex.mWeights[4] > 0 && vertex.mWeights[8] > 0 && vertex.mWeights[15] > 0,
            "Fifth/ninth/sixteenth influences lost or groups normalized independently");
    }
}
