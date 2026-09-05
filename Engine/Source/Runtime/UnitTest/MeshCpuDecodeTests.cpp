#include "Hepch.h"

#include "Runtime/UnitTest/MeshCpuDecodeTests.h"
#include "Runtime/Graphics/Mesh/MeshLoader.h"
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
                skinnedShared.SkinnedVertices[0].mWeights[0] == 0.4f &&
                skinnedShared.SkinnedVertices[0].mWeights[3] == 0.1f,
                "Skin joint or weight data changed during CPU decode.");

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
        TemporaryDirectory temporaryDirectory;
        const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
        std::filesystem::create_directories(projectRoot / "Assets/Models");

        const MeshFixture fixture = MakeMeshFixture();
        const std::filesystem::path meshPath = projectRoot / "Assets/Models/CpuDecodeFixture.hmesh";
        WriteBytes(projectRoot / "Assets/Models/CpuDecodeFixture.bin", fixture.BufferBytes);
        WriteMeshResource(meshPath, fixture.Resource);

        VerifyValidDecode(meshPath, projectRoot);
        VerifyFailurePaths(projectRoot, meshPath, fixture);
		VerifyExplicitMeshKeyContract(projectRoot);
    }
}
