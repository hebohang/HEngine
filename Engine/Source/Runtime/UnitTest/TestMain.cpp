#include "Hepch.h"

#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Meta/Reflection/ReflectionRegister.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/Core/SimulationClock.h"
#include "Runtime/Core/PresentationClock.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/Graphics/Mesh/MeshHitRange.h"
#include "Runtime/Graphics/Mesh/MeshResourceValidator.h"
#include "Runtime/Platform/PlatformProcess.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/UnitTest/AntiAliasingTests.h"
#include "Runtime/UnitTest/AudioLifecycleTests.h"
#include "Runtime/UnitTest/BindlessDescriptorManagerTests.h"
#include "Runtime/UnitTest/ConfigManagerTests.h"
#include "Runtime/UnitTest/CornellSceneSerializationTests.h"
#include "Runtime/UnitTest/DevelopmentLaunchPathsTests.h"
#include "Runtime/UnitTest/DevCommandTests.h"
#include "Runtime/UnitTest/EffectTransformBatchTests.h"
#include "Runtime/UnitTest/EffekseerRegressionManifestTests.h"
#include "Runtime/UnitTest/EffekseerRuntimeLifecycleTests.h"
#include "Runtime/UnitTest/GamepadInputTests.h"
#include "Runtime/UnitTest/HardwareRayTracingTests.h"
#if HE_WITH_OFFLINE_REFERENCE_RENDERER_TESTS
#include "Plugins/OfflineReferenceRenderer/Tests/OfflineReferenceRendererSessionTests.h"
#endif
#include "Runtime/UnitTest/MaterialHotReloadTests.h"
#include "Runtime/UnitTest/MaterialParameterPackingTests.h"
#include "Runtime/UnitTest/MaterialPipelineKeyTests.h"
#include "Runtime/UnitTest/MaterialResourceCacheTests.h"
#include "Runtime/UnitTest/MaterialThumbnailCacheKeyTests.h"
#include "Runtime/UnitTest/MaterialSerializationTests.h"
#include "Runtime/UnitTest/MeshCpuDecodeTests.h"
#include "Runtime/UnitTest/PluginSystemTests.h"
#include "Runtime/UnitTest/RenderCapabilityPolicyTests.h"
#include "Runtime/UnitTest/RenderGraphAccelerationStructureTests.h"
#include "Runtime/UnitTest/RenderGraphResourcePoolTests.h"
#include "Runtime/UnitTest/RenderGraphTests.h"
#include "Runtime/UnitTest/RenderSettingsTests.h"
#include "Runtime/UnitTest/RenderRhiContractTests.h"
#include "Runtime/UnitTest/RenderWorldSubmissionTests.h"
#include "Runtime/UnitTest/ViewportRendererExtensionTests.h"
#include "Runtime/UnitTest/RuntimePrefabResourceTests.h"
#include "Runtime/UnitTest/RuntimeResourceCatalogTests.h"
#include "Runtime/UnitTest/RuntimeResourceRegistryTests.h"
#include "Runtime/UnitTest/RuntimeResourceWarmupTests.h"
#include "Runtime/UnitTest/RuntimeSceneRequestTests.h"
#include "Runtime/UnitTest/RuntimeUiResourceTests.h"
#include "Runtime/UnitTest/RmlUiPointerPayloadTests.h"
#include "Runtime/UnitTest/SceneDocumentTests.h"
#include "Runtime/UnitTest/SystemLifecycleTests.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "_generated/serializer/all_serializer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace HEngine::Tests
{
	void ReflectedResourceSchemaCompatibilityContract();
}

namespace
{
	using TestFunction = void(*)();

	struct TestCase
	{
		std::string_view Name;
		TestFunction Run;
		bool bRequiresReflection = false;
	};

	class ReflectionScope
	{
	public:
		ReflectionScope()
		{
			HEngine::Reflection::TypeMetaRegister::metaRegister();
		}

		~ReflectionScope()
		{
			HEngine::Reflection::TypeMetaRegister::metaUnregister();
		}
	};

	void RunTest(const TestCase& test)
	{
		if (test.bRequiresReflection)
		{
			ReflectionScope reflectionScope;
			test.Run();
			return;
		}

		test.Run();
	}

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
				("HEngineTests-" + std::to_string(timestamp) + "-" +
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

	void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		Require(file.is_open(), "Failed to create a binary test fixture.");
		file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		file.close();
		Require(!file.fail(), "Failed to write a binary test fixture.");
	}

	void PlatformProcessRelaunch()
	{
		TemporaryDirectory directory;
		const std::filesystem::path marker =
			directory.GetPath() / "relaunch marker with spaces.txt";
		const std::array<std::string, 2> arguments = {
			"--platform-process-relaunch-child",
			marker.string(),
		};
		const HEngine::PlatformProcessLaunchResult launch =
			HEngine::PlatformProcess::RelaunchCurrentProcess(arguments);
		Require(launch.bSuccess,
			"PlatformProcess could not relaunch the current executable: " +
				launch.Error);

		constexpr auto timeout = std::chrono::seconds(5);
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		std::string contents;
		do
		{
			std::ifstream stream(marker, std::ios::binary);
			contents.assign(
				std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>());
			if (contents == "HEngine process relaunch child")
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		while (std::chrono::steady_clock::now() < deadline);

		Require(contents == "HEngine process relaunch child",
			"The relaunched child did not receive its quoted marker path.");
	}

	HEngine::MeshRes MakeTriangleMeshResource()
	{
		using namespace HEngine;

		const std::array<float, 9> positions = {
			0.0f, 0.0f, 0.0f,
			1.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f
		};
		const std::array<float, 9> normals = {
			0.0f, 0.0f, 1.0f,
			0.0f, 0.0f, 1.0f,
			0.0f, 0.0f, 1.0f
		};
		const std::array<std::uint16_t, 3> indices = { 0, 1, 2 };

		MeshRes resource;
		resource.name = "Triangle";
		resource.bufferVec.resize(1);
		MeshBuffer& buffer = resource.bufferVec.front();
		buffer.name = "Triangle";
		buffer.uri = "Assets/Triangle.bin";
		buffer.byteLength = static_cast<int>(
			sizeof(positions) + sizeof(normals) + sizeof(indices));
		buffer.data.resize(static_cast<std::size_t>(buffer.byteLength));
		std::size_t writeOffset = 0;
		std::memcpy(buffer.data.data() + writeOffset, positions.data(), sizeof(positions));
		writeOffset += sizeof(positions);
		std::memcpy(buffer.data.data() + writeOffset, normals.data(), sizeof(normals));
		writeOffset += sizeof(normals);
		std::memcpy(buffer.data.data() + writeOffset, indices.data(), sizeof(indices));

		resource.bufferViewVec = {
			{ .name = "Positions", .bufferIndex = 0, .byteOffset = 0,
				.byteLength = sizeof(positions) },
			{ .name = "Normals", .bufferIndex = 0, .byteOffset = sizeof(positions),
				.byteLength = sizeof(normals) },
			{ .name = "Indices", .bufferIndex = 0,
				.byteOffset = sizeof(positions) + sizeof(normals),
				.byteLength = sizeof(indices) }
		};
		resource.accessorVec = {
			{ .bufferViewIndex = 0, .name = "Positions", .componentType = HENGINE_COMPONENT_TYPE_FLOAT,
				.count = positions.size() / 3, .type = "VEC3" },
			{ .bufferViewIndex = 1, .name = "Normals", .componentType = HENGINE_COMPONENT_TYPE_FLOAT,
				.count = normals.size() / 3, .type = "VEC3" },
			{ .bufferViewIndex = 2, .name = "Indices",
				.componentType = HENGINE_COMPONENT_TYPE_UNSIGNED_SHORT,
				.count = indices.size(), .type = "SCALAR" }
		};
		Primitive primitive;
		primitive.attributes["POSITION"] = 0;
		primitive.attributes["NORMAL"] = 1;
		primitive.indices = 2;
		resource.primitives.push_back(std::move(primitive));
		return resource;
	}

	void StrictProjectRelativePath()
	{
		using namespace HEngine::Utils;

		std::string normalizedPath;
		PathValidationResult result = TryNormalizeProjectRelativePath(
			"Assets\\Models//Cube/./Cube.hmesh",
			normalizedPath);
		Require(static_cast<bool>(result), "A valid mixed-separator project path was rejected.");
		Require(normalizedPath == "Assets/Models/Cube/Cube.hmesh",
			"A project path was not normalized to a stable generic form.");

		const std::array<std::pair<std::string_view, PathValidationErrorCode>, 13> invalidPaths = {{
			{ "", PathValidationErrorCode::EmptyPath },
			{ "../outside.bin", PathValidationErrorCode::ParentTraversal },
			{ "Assets/a/../../outside.bin", PathValidationErrorCode::ParentTraversal },
			{ "C:\\outside.bin", PathValidationErrorCode::AbsolutePath },
			{ "C:outside.bin", PathValidationErrorCode::AbsolutePath },
			{ "\\\\server\\share\\outside.bin", PathValidationErrorCode::AbsolutePath },
			{ "/outside.bin", PathValidationErrorCode::AbsolutePath },
			{ "Assets/file.bin:stream", PathValidationErrorCode::InvalidComponent },
			{ "Assets/CON/file.bin", PathValidationErrorCode::ReservedComponent },
			{ "Assets/CONIN$/file.bin", PathValidationErrorCode::ReservedComponent },
			{ "Assets/CONOUT$/file.bin", PathValidationErrorCode::ReservedComponent },
			{ "Assets/COM\xC2\xB9/file.bin", PathValidationErrorCode::ReservedComponent },
			{ "Assets/LPT\xC2\xB3/file.bin", PathValidationErrorCode::ReservedComponent }
		}};
		for (const auto& [input, expectedCode] : invalidPaths)
		{
			normalizedPath = "sentinel";
			result = TryNormalizeProjectRelativePath(input, normalizedPath);
			Require(!result && result.Code == expectedCode,
				"An unsafe project path returned the wrong validation result.");
			Require(normalizedPath.empty(), "A failed path validation retained output state.");
		}

		std::string embeddedNull("Assets/Bad\0Name.bin", 19);
		result = TryNormalizeProjectRelativePath(embeddedNull, normalizedPath);
		Require(!result && result.Code == PathValidationErrorCode::EmbeddedNull,
			"An embedded null was not rejected.");

		std::string firstPath;
		std::string secondPath;
		Require(static_cast<bool>(TryNormalizeProjectRelativePath("Assets/Foo\\Bar.hmesh", firstPath)),
			"The first cache-key path was rejected.");
		Require(static_cast<bool>(TryNormalizeProjectRelativePath("assets/foo/./bar.hmesh", secondPath)),
			"The second cache-key path was rejected.");
		Require(MakeProjectPathCacheKey(firstPath) == MakeProjectPathCacheKey(secondPath),
			"Equivalent Windows asset paths generated different cache keys.");

		TemporaryDirectory temporaryDirectory;
		const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / L"Project-资源";
		std::filesystem::create_directories(projectRoot / "Assets" / "Models");
		std::filesystem::path resolvedPath;
		result = TryResolveProjectRelativePath(
			projectRoot,
			"Assets/Models/Cube.hmesh",
			resolvedPath,
			normalizedPath);
		Require(static_cast<bool>(result), "A valid project path could not be resolved.");
		Require(resolvedPath == projectRoot / "Assets" / "Models" / "Cube.hmesh",
			"A project path resolved to the wrong location.");
	}

	void AtomicResourceSave()
	{
		TemporaryDirectory temporaryDirectory;
		const std::filesystem::path saveDirectory = temporaryDirectory.GetPath() / L"资源";
		std::filesystem::create_directories(saveDirectory);
		const std::filesystem::path targetPath = saveDirectory / L"原子资源.json";

		HEngine::Json firstJson = { { "value", "first" } };
		HEngine::Utils::SaveJsonToFile(firstJson, targetPath);
		Require(HEngine::Utils::LoadJsonFile(targetPath) == firstJson,
			"The initial atomic JSON save did not round-trip.");

		HEngine::Json replacementJson = { { "value", "replacement" } };
		HEngine::Utils::SaveJsonToFile(replacementJson, targetPath);
		Require(HEngine::Utils::LoadJsonFile(targetPath) == replacementJson,
			"Atomic JSON replacement did not publish the new document.");

		bool bSerializationFailed = false;
		try
		{
			HEngine::Json invalidJson = std::string(1, static_cast<char>(0xff));
			HEngine::Utils::SaveJsonToFile(invalidJson, targetPath);
		}
		catch (const std::exception&)
		{
			bSerializationFailed = true;
		}
		Require(bSerializationFailed, "Invalid UTF-8 did not fail JSON serialization.");
		Require(HEngine::Utils::LoadJsonFile(targetPath) == replacementJson,
			"A serialization failure modified the previous asset.");

		const std::filesystem::path directoryTarget = saveDirectory / "ExistingDirectory";
		std::filesystem::create_directory(directoryTarget);
		const std::filesystem::path sentinelPath = directoryTarget / "sentinel.txt";
		WriteBytes(sentinelPath, { 's', 'a', 'f', 'e' });
		bool bReplaceFailed = false;
		try
		{
			HEngine::Utils::SaveJsonToFile(firstJson, directoryTarget);
		}
		catch (const std::exception&)
		{
			bReplaceFailed = true;
		}
		Require(bReplaceFailed, "Replacing a directory unexpectedly succeeded.");
		Require(std::filesystem::is_directory(directoryTarget) && std::filesystem::exists(sentinelPath),
			"A failed atomic replacement damaged its existing target.");

		std::size_t entryCount = 0;
		for (const auto& entry : std::filesystem::directory_iterator(saveDirectory))
		{
			(void)entry;
			++entryCount;
		}
		Require(entryCount == 2, "Atomic save left a temporary sibling behind.");
	}

	void GroupedAssetOutputNames()
	{
		using namespace HEngine;

		TemporaryDirectory temporaryDirectory;
		const std::filesystem::path outputDirectory = temporaryDirectory.GetPath() / "Assets";
		std::filesystem::create_directories(outputDirectory);

		std::filesystem::path stem = AssetManager::GetAvailableFileStem(
			"Hero",
			outputDirectory,
			{ ".bin", ".hmesh" });
		Require(stem == outputDirectory / "Hero",
			"An unused grouped asset stem was unexpectedly renamed.");

		const std::vector<std::uint8_t> sentinel = { 's', 'a', 'f', 'e' };
		const std::filesystem::path existingBinary = outputDirectory / "Hero.bin";
		WriteBytes(existingBinary, sentinel);
		stem = AssetManager::GetAvailableFileStem("Hero", outputDirectory, { ".bin", ".hmesh" });
		Require(stem == outputDirectory / "Hero_0",
			"A binary collision did not advance the whole asset group.");

		WriteBytes(outputDirectory / "Hero_0.hmesh", {});
		stem = AssetManager::GetAvailableFileStem("Hero", outputDirectory, { ".bin", ".hmesh" });
		Require(stem == outputDirectory / "Hero_1",
			"A metadata collision did not advance the whole asset group.");

		bool bCreateNewFailed = false;
		try
		{
			const std::vector<std::uint8_t> replacement = { 'b', 'a', 'd' };
			HEngine::Utils::WriteBytesToFileAtomically(
				existingBinary,
				replacement.data(),
				replacement.size(),
				HEngine::Utils::AtomicWriteMode::CreateNew);
		}
		catch (const std::exception&)
		{
			bCreateNewFailed = true;
		}
		Require(bCreateNewFailed, "CreateNew unexpectedly overwrote an existing grouped asset.");

		std::ifstream oldBinary(existingBinary, std::ios::binary);
		const std::vector<std::uint8_t> persisted(
			(std::istreambuf_iterator<char>(oldBinary)),
			std::istreambuf_iterator<char>());
		Require(persisted == sentinel, "A grouped asset collision modified the previous binary.");
	}

	void MeshResourceValidation()
	{
		using namespace HEngine;

		MeshRes validResource = MakeTriangleMeshResource();
		MeshRes schemaResource = validResource;
		schemaResource.bufferVec[0].data.clear();
		Require(static_cast<bool>(MeshResourceValidator::ValidateSchema(schemaResource)),
			"A valid mesh schema was rejected.");
		Require(static_cast<bool>(MeshResourceValidator::ValidateLoaded(validResource)),
			"A valid loaded mesh was rejected.");
		Require(!MeshResourceValidator::ValidateSchema(validResource),
			"Serialized runtime mesh buffer data was accepted as schema input.");

		MeshRes invalidResource = schemaResource;
		invalidResource.bufferVec[0].byteLength = -1;
		Require(!MeshResourceValidator::ValidateSchema(invalidResource),
			"A negative mesh buffer length was accepted.");

		invalidResource = validResource;
		invalidResource.bufferVec[0].data.pop_back();
		Require(!MeshResourceValidator::ValidateLoaded(invalidResource),
			"A short loaded mesh buffer was accepted.");

		invalidResource = schemaResource;
		invalidResource.bufferViewVec[0].byteOffset = std::numeric_limits<std::size_t>::max();
		Require(!MeshResourceValidator::ValidateSchema(invalidResource),
			"An overflowing mesh buffer view was accepted.");

		invalidResource = schemaResource;
		invalidResource.bufferViewVec[0].byteStride = 12;
		Require(!MeshResourceValidator::ValidateSchema(invalidResource),
			"An unsupported interleaved mesh stream was accepted.");

		invalidResource = schemaResource;
		invalidResource.primitives[0].attributes.erase("POSITION");
		Require(!MeshResourceValidator::ValidateSchema(invalidResource),
			"A mesh primitive without POSITION was accepted.");

		invalidResource = schemaResource;
		invalidResource.accessorVec[2].componentType = HENGINE_COMPONENT_TYPE_SHORT;
		Require(!MeshResourceValidator::ValidateSchema(invalidResource),
			"A signed mesh index stream was accepted.");

		invalidResource = validResource;
		const std::uint16_t outOfRangeIndex = 3;
		std::memcpy(
			invalidResource.bufferVec[0].data.data() + invalidResource.bufferViewVec[2].byteOffset,
			&outOfRangeIndex,
			sizeof(outOfRangeIndex));
		const MeshValidationResult indexResult = MeshResourceValidator::ValidateLoaded(invalidResource);
		Require(!indexResult && indexResult.Code == MeshValidationError::IndexOutOfRange,
			"An out-of-range mesh index was accepted.");

		invalidResource = validResource;
		const float infiniteVertex = std::numeric_limits<float>::infinity();
		std::memcpy(invalidResource.bufferVec[0].data.data(), &infiniteVertex, sizeof(infiniteVertex));
		const MeshValidationResult nonFiniteResult = MeshResourceValidator::ValidateLoaded(invalidResource);
		Require(!nonFiniteResult && nonFiniteResult.Code == MeshValidationError::InvalidVertexData,
			"A non-finite mesh vertex value was accepted.");

		TemporaryDirectory temporaryDirectory;
		const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
		std::filesystem::create_directories(projectRoot / "Assets");
		const std::filesystem::path binaryPath = projectRoot / "Assets" / "Triangle.bin";
		WriteBytes(binaryPath, validResource.bufferVec[0].data);

		MeshRes fileResource = schemaResource;
		Require(static_cast<bool>(MeshResourceValidator::LoadAndValidateBuffers(fileResource, projectRoot)),
			"A valid mesh binary file failed load validation.");

		fileResource = schemaResource;
		fileResource.bufferVec[0].uri = "../Triangle.bin";
		const MeshValidationResult unsafePathResult =
			MeshResourceValidator::LoadAndValidateBuffers(fileResource, projectRoot);
		Require(!unsafePathResult && unsafePathResult.Code == MeshValidationError::InvalidBufferPath,
			"A mesh buffer traversal path was accepted.");

		fileResource = schemaResource;
		fileResource.skeletonPath = "../Outside.hskel";
		const MeshValidationResult unsafeReferenceResult =
			MeshResourceValidator::LoadAndValidateBuffers(fileResource, projectRoot);
		Require(!unsafeReferenceResult &&
			unsafeReferenceResult.Code == MeshValidationError::InvalidReferencedPath,
			"A mesh skeleton traversal path was accepted.");

		std::vector<std::uint8_t> shortData = validResource.bufferVec[0].data;
		shortData.pop_back();
		WriteBytes(binaryPath, shortData);
		fileResource = schemaResource;
		const MeshValidationResult shortFileResult =
			MeshResourceValidator::LoadAndValidateBuffers(fileResource, projectRoot);
		Require(!shortFileResult && shortFileResult.Code == MeshValidationError::BufferSizeMismatch,
			"A short mesh binary file was accepted.");
	}

	void PbrLightingReflection()
	{
		HEngine::PointLightComponent defaultPointLight;
		Require(defaultPointLight.FalloffExponent == 2.0f,
			"Point light must default to inverse-square attenuation.");

		HEngine::PointLightComponent sourcePointLight(
			640.0f, glm::vec3(0.8f, 0.6f, 0.4f), 12.0f, 3.5f);
		HEngine::Json pointLightJson;
		HEngine::Serializer::Serialize(pointLightJson, sourcePointLight);
		Require(pointLightJson.contains("FalloffExponent"),
			"Point light attenuation exponent was not serialized.");

		HEngine::PointLightComponent roundTrippedPointLight;
		HEngine::Serializer::Deserialize(pointLightJson, roundTrippedPointLight);
		Require(roundTrippedPointLight.FalloffExponent == sourcePointLight.FalloffExponent,
			"Point light attenuation exponent did not round-trip.");

		HEngine::EnvironmentLightComponent environmentLight;
		HEngine::Json environmentJson;
		HEngine::Serializer::Serialize(environmentJson, environmentLight);
		Require(!environmentJson.contains("SkyBoxLod"),
			"Environment serialization still exposes SkyBoxLod.");
		Require(!environmentJson.contains("MaxReflectionLod"),
			"Environment serialization still exposes MaxReflectionLod.");
	}

	void EmbreeSubMeshPickResolve()
	{
		const std::vector<HEngine::MeshHitRange> hitRanges = {
			{ .SubMeshIndex = 0, .MaterialIndex = 2, .FirstPrimID = 0,  .PrimCount = 10 },
			{ .SubMeshIndex = 1, .MaterialIndex = 5, .FirstPrimID = 10, .PrimCount = 4  },
			{ .SubMeshIndex = 2, .MaterialIndex = 1, .FirstPrimID = 14, .PrimCount = 8  }
		};

		std::uint32_t subMeshIndex = 0;
		std::uint32_t materialIndex = 0;
		Require(HEngine::ResolveMeshHitRange(hitRanges, 0, subMeshIndex, materialIndex),
			"Primitive 0 did not resolve.");
		Require(subMeshIndex == 0 && materialIndex == 2, "Primitive 0 resolved to the wrong range.");
		Require(HEngine::ResolveMeshHitRange(hitRanges, 11, subMeshIndex, materialIndex),
			"Primitive 11 did not resolve.");
		Require(subMeshIndex == 1 && materialIndex == 5, "Primitive 11 resolved to the wrong range.");
		Require(HEngine::ResolveMeshHitRange(hitRanges, 21, subMeshIndex, materialIndex),
			"Primitive 21 did not resolve.");
		Require(subMeshIndex == 2 && materialIndex == 1, "Primitive 21 resolved to the wrong range.");
		Require(!HEngine::ResolveMeshHitRange(hitRanges, 22, subMeshIndex, materialIndex),
			"Primitive 22 unexpectedly resolved.");

		const std::uint32_t maxPrimitiveID = std::numeric_limits<std::uint32_t>::max();
		const std::vector<HEngine::MeshHitRange> boundaryRanges = {
			{ .SubMeshIndex = 7, .MaterialIndex = 9, .FirstPrimID = maxPrimitiveID - 1, .PrimCount = 3 }
		};
		Require(HEngine::ResolveMeshHitRange(
			boundaryRanges, maxPrimitiveID, subMeshIndex, materialIndex),
			"Maximum primitive ID did not resolve without overflow.");
		Require(subMeshIndex == 7 && materialIndex == 9,
			"Maximum primitive ID resolved to the wrong range.");
	}

	void FixedSimulationClock()
	{
		auto& clock = HEngine::SimulationClock::GetInstance();
		clock.Reset();
		Require(clock.GetFrameNumber() == 0 && clock.GetCompletedFrameCount() == 0,
			"A reset simulation clock must not expose a frame.");

		const HEngine::Timestep firstStep = clock.BeginFrame(0.125f);
		Require(firstStep.GetSeconds() == 0.125f,
			"The fixed simulation delta was not applied to the first frame.");
		Require(clock.GetFrameNumber() == 1 && clock.GetCompletedFrameCount() == 0,
			"The active frame numbering is not one-based.");
		clock.CompleteFrame();

		const HEngine::Timestep secondStep = clock.BeginFrame(0.125f);
		Require(secondStep.GetSeconds() == 0.125f,
			"The fixed simulation delta changed between frames.");
		Require(clock.GetFrameNumber() == 2 && clock.GetCompletedFrameCount() == 1,
			"The completed frame count advanced at the wrong boundary.");
		clock.CompleteFrame();

		Require(clock.GetCompletedFrameCount() == 2,
			"The simulation clock did not commit the second frame.");
		Require(std::abs(clock.GetElapsedSeconds() - 0.25) < 0.000001,
			"The simulation clock elapsed time is not deterministic.");

		clock.SetPaused(true);
		const HEngine::Timestep pausedStep = clock.BeginFrame(0.125f);
		Require(pausedStep.GetSeconds() == 0.0f,
			"A paused simulation clock advanced its timestep.");
		clock.CompleteFrame();
		Require(std::abs(clock.GetElapsedSeconds() - 0.25) < 0.000001,
			"A paused simulation clock advanced elapsed simulation time.");

		clock.SetPaused(false);
		const HEngine::Timestep presentationStep = clock.BeginFrame(0.125f, false);
		Require(presentationStep.GetSeconds() == 0.0f,
			"A presentation-only frame advanced its timestep.");
		clock.CompleteFrame();
		Require(std::abs(clock.GetElapsedSeconds() - 0.25) < 0.000001,
			"A presentation-only frame advanced elapsed simulation time.");
	}

	void PresentationClockContract()
	{
		auto& presentationClock = HEngine::PresentationClock::GetInstance();
		auto& simulationClock = HEngine::SimulationClock::GetInstance();
		presentationClock.Reset();
		simulationClock.Reset();

		Require(presentationClock.GetFrameNumber() == 0 &&
			presentationClock.GetCompletedFrameCount() == 0 &&
			presentationClock.GetElapsedSeconds() == 0.0,
			"A reset presentation clock must start from a clean boundary.");

		const HEngine::Timestep firstStep = presentationClock.BeginFrame(1.0f / 60.0f);
		Require(std::abs(firstStep.GetSeconds() - (1.0f / 60.0f)) < 0.000001f,
			"The deterministic presentation delta was not applied.");
		presentationClock.CompleteFrame();

		simulationClock.SetPaused(true);
		const HEngine::Timestep pausedSimulationStep = simulationClock.BeginFrame(1.0f / 60.0f);
		const HEngine::Timestep pausedPresentationStep = presentationClock.BeginFrame(1.0f / 60.0f);
		Require(pausedSimulationStep.GetSeconds() == 0.0f,
			"The simulation clock advanced while paused.");
		Require(pausedPresentationStep.GetSeconds() > 0.0f,
			"The presentation clock was incorrectly coupled to gameplay pause.");
		simulationClock.CompleteFrame();
		presentationClock.CompleteFrame();

		const double elapsedBeforeSuspend = presentationClock.GetElapsedSeconds();
		const HEngine::Timestep suspendedStep = presentationClock.BeginFrame(1.0f / 60.0f, false);
		Require(suspendedStep.GetSeconds() == 0.0f,
			"A suspended presentation frame advanced.");
		presentationClock.CompleteFrame();
		Require(std::abs(presentationClock.GetElapsedSeconds() - elapsedBeforeSuspend) < 0.000001,
			"Suspension changed presentation elapsed time.");

		const HEngine::Timestep resumedStep = presentationClock.BeginFrame(1.0f / 120.0f);
		Require(std::abs(resumedStep.GetSeconds() - (1.0f / 120.0f)) < 0.000001f,
			"The presentation clock did not resume continuously with the requested fixed step.");
		presentationClock.CompleteFrame();

		const HEngine::Timestep clampedStep = presentationClock.BeginFrame(5.0f);
		Require(std::abs(clampedStep.GetSeconds() - HEngine::PresentationClock::GetMaximumDeltaSeconds()) < 0.000001f,
			"A pathological presentation frame was not clamped.");
		presentationClock.CompleteFrame();

		presentationClock.Reset();
		Require(presentationClock.GetFrameNumber() == 0 &&
			presentationClock.GetCompletedFrameCount() == 0 &&
			presentationClock.GetDeltaSeconds() == 0.0f &&
			presentationClock.GetElapsedSeconds() == 0.0,
			"Reset did not clear all presentation clock state.");
		simulationClock.Reset();
	}

	void ApplicationRuntimeStartupGuard()
	{
		auto& application = HEngine::Application::GetInstance();
		auto& clock = HEngine::SimulationClock::GetInstance();
		application.CancelRuntimeStartup();
		clock.Reset();

		application.BeginRuntimeStartup();
		Require(application.IsRuntimeStartupPending(),
			"Beginning runtime startup did not close the automation gate.");
		Require(!application.HasRuntimeStartupPresentedFrame(),
			"A fresh runtime startup incorrectly reported a presented frame.");
		Require(!application.HasRuntimeStartupGameplayPresentedFrame(),
			"A fresh runtime startup incorrectly reported a presented gameplay frame.");

		bool bRejectedReentry = false;
		try
		{
			application.BeginRuntimeStartup();
		}
		catch (const std::logic_error&)
		{
			bRejectedReentry = true;
		}
		Require(bRejectedReentry && application.IsRuntimeStartupPending(),
			"Runtime startup reentry did not preserve the active gate.");

		(void)clock.BeginFrame(0.125f);
		bool bRejectedActiveFrameCompletion = false;
		try
		{
			application.CompleteRuntimeStartup();
		}
		catch (const std::logic_error&)
		{
			bRejectedActiveFrameCompletion = true;
		}
		Require(bRejectedActiveFrameCompletion && application.IsRuntimeStartupPending(),
			"Completing startup during an active frame changed the gate.");
		clock.CompleteFrame();

		bool bRejectedCompletionBeforePresent = false;
		try
		{
			application.CompleteRuntimeStartup();
		}
		catch (const std::logic_error&)
		{
			bRejectedCompletionBeforePresent = true;
		}
		Require(bRejectedCompletionBeforePresent && application.IsRuntimeStartupPending(),
			"Completing startup before its first Present changed the gate.");

		bool bRejectedGameplayRequestBeforePresent = false;
		try
		{
			application.RequestRuntimeStartupGameplayPresent();
		}
		catch (const std::logic_error&)
		{
			bRejectedGameplayRequestBeforePresent = true;
		}
		Require(bRejectedGameplayRequestBeforePresent && application.IsRuntimeStartupPending(),
			"Requesting a gameplay Present before the first startup Present changed the gate.");

		application.NotifyRuntimeStartupPresentedFrame();
		Require(application.HasRuntimeStartupPresentedFrame(),
			"Recording the first runtime startup Present did not update the gate.");

		bool bRejectedDuplicatePresent = false;
		try
		{
			application.NotifyRuntimeStartupPresentedFrame();
		}
		catch (const std::logic_error&)
		{
			bRejectedDuplicatePresent = true;
		}
		Require(bRejectedDuplicatePresent && application.HasRuntimeStartupPresentedFrame(),
			"Recording the first runtime startup Present twice changed the gate.");

		bool bRejectedCompletionBeforeGameplayPresent = false;
		try
		{
			application.CompleteRuntimeStartup();
		}
		catch (const std::logic_error&)
		{
			bRejectedCompletionBeforeGameplayPresent = true;
		}
		Require(bRejectedCompletionBeforeGameplayPresent && application.IsRuntimeStartupPending(),
			"Completing startup before a gameplay Present changed the gate.");

		bool bRejectedGameplayPresentBeforeRequest = false;
		try
		{
			application.NotifyRuntimeStartupGameplayPresentedFrame();
		}
		catch (const std::logic_error&)
		{
			bRejectedGameplayPresentBeforeRequest = true;
		}
		Require(bRejectedGameplayPresentBeforeRequest &&
			!application.HasRuntimeStartupGameplayPresentedFrame(),
			"Recording a gameplay Present before requesting it changed the gate.");

		(void)clock.BeginFrame(0.125f, false);
		bool bRejectedGameplayRequestDuringFrame = false;
		try
		{
			application.RequestRuntimeStartupGameplayPresent();
		}
		catch (const std::logic_error&)
		{
			bRejectedGameplayRequestDuringFrame = true;
		}
		Require(bRejectedGameplayRequestDuringFrame && application.IsRuntimeStartupPending(),
			"Requesting a gameplay Present during an active frame changed the gate.");
		clock.CompleteFrame();

		application.RequestRuntimeStartupGameplayPresent();
		bool bRejectedDuplicateGameplayRequest = false;
		try
		{
			application.RequestRuntimeStartupGameplayPresent();
		}
		catch (const std::logic_error&)
		{
			bRejectedDuplicateGameplayRequest = true;
		}
		Require(bRejectedDuplicateGameplayRequest &&
			!application.HasRuntimeStartupGameplayPresentedFrame(),
			"Requesting a gameplay Present twice changed the gate.");

		const HEngine::Timestep startupPresentationStep = clock.BeginFrame(0.125f, false);
		Require(startupPresentationStep.GetSeconds() == 0.0f,
			"The startup gameplay presentation frame advanced simulation time.");
		application.NotifyRuntimeStartupGameplayPresentedFrame();
		Require(application.HasRuntimeStartupGameplayPresentedFrame(),
			"Recording the runtime startup gameplay Present did not update the gate.");

		bool bRejectedDuplicateGameplayPresent = false;
		try
		{
			application.NotifyRuntimeStartupGameplayPresentedFrame();
		}
		catch (const std::logic_error&)
		{
			bRejectedDuplicateGameplayPresent = true;
		}
		Require(bRejectedDuplicateGameplayPresent &&
			application.HasRuntimeStartupGameplayPresentedFrame(),
			"Recording the runtime startup gameplay Present twice changed the gate.");

		bool bRejectedGameplayCompletionDuringFrame = false;
		try
		{
			application.CompleteRuntimeStartup();
		}
		catch (const std::logic_error&)
		{
			bRejectedGameplayCompletionDuringFrame = true;
		}
		Require(bRejectedGameplayCompletionDuringFrame && application.IsRuntimeStartupPending(),
			"Completing startup during the gameplay presentation frame changed the gate.");
		clock.CompleteFrame();

		application.CompleteRuntimeStartup();
		Require(!application.IsRuntimeStartupPending() &&
			!application.HasRuntimeStartupPresentedFrame() &&
			!application.HasRuntimeStartupGameplayPresentedFrame(),
			"Successful runtime startup completion did not open the automation gate.");
		Require(clock.GetFrameNumber() == 0 && clock.GetCompletedFrameCount() == 0,
			"Successful runtime startup completion did not reset the gameplay clock.");

		const HEngine::Timestep firstGameplayStep = clock.BeginFrame(0.125f);
		Require(firstGameplayStep.GetSeconds() == 0.125f &&
			clock.GetFrameNumber() == 1 && clock.GetCompletedFrameCount() == 0,
			"The first post-startup automation frame was not gameplay frame one.");
		clock.CompleteFrame();

		bool bRejectedPresentWithoutStartup = false;
		try
		{
			application.NotifyRuntimeStartupPresentedFrame();
		}
		catch (const std::logic_error&)
		{
			bRejectedPresentWithoutStartup = true;
		}
		Require(bRejectedPresentWithoutStartup,
			"A runtime startup Present was accepted without an active startup.");

		bool bRejectedGameplayPresentWithoutStartup = false;
		try
		{
			application.NotifyRuntimeStartupGameplayPresentedFrame();
		}
		catch (const std::logic_error&)
		{
			bRejectedGameplayPresentWithoutStartup = true;
		}
		Require(bRejectedGameplayPresentWithoutStartup,
			"A runtime startup gameplay Present was accepted without an active startup.");

		application.CancelRuntimeStartup();
		application.CancelRuntimeStartup();
		Require(!application.IsRuntimeStartupPending() &&
			!application.HasRuntimeStartupPresentedFrame() &&
			!application.HasRuntimeStartupGameplayPresentedFrame(),
			"Cancelling runtime startup was not idempotent.");
		clock.Reset();
	}

	constexpr auto kTests = std::to_array<TestCase>({
		{ "StrictProjectRelativePath", &StrictProjectRelativePath },
		{ "AtomicResourceSave", &AtomicResourceSave },
		{ "AntiAliasing", &HEngine::Tests::AntiAliasingContract },
		{ "AudioLifecycle", &HEngine::Tests::AudioLifecycleContract },
		{ "DescriptorIndexAllocator", &HEngine::Tests::DescriptorIndexAllocatorContract },
		{ "ConfigManagerDualRoot", &HEngine::Tests::ConfigManagerDualRootContract },
		{ "DevelopmentLaunchPaths", &HEngine::Tests::DevelopmentLaunchPathsContract },
		{ "CornellSceneSerialization", &HEngine::Tests::CornellSceneSerializationContract, true },
		{ "DevCommandPlatform", &HEngine::Tests::DevCommandPlatformContract },
		{ "EffectTransformBatch", &HEngine::Tests::EffectTransformBatchContract },
		{ "EffekseerRegressionManifest", &HEngine::Tests::EffekseerRegressionManifestContract },
		{ "EffekseerRuntimeLifecycle", &HEngine::Tests::EffekseerRuntimeLifecycleContract },
		{ "GamepadInputState", &HEngine::Tests::GamepadInputStateContract },
		{ "HardwareRayTracing", &HEngine::Tests::HardwareRayTracingContract },
#if HE_WITH_OFFLINE_REFERENCE_RENDERER_TESTS
		{ "OfflineReferenceRendererSession", &HEngine::Tests::OfflineReferenceRendererSessionContract },
#endif
		{ "MaterialHotReload", &HEngine::Tests::MaterialHotReloadContract },
		{ "MaterialParameterPacking", &HEngine::Tests::MaterialParameterPackingContract },
		{ "MaterialPipelineKey", &HEngine::Tests::MaterialPipelineKeyContract },
		{ "MaterialResourceCache", &HEngine::Tests::MaterialResourceCacheContract },
		{ "MaterialThumbnailCacheKey", &HEngine::Tests::MaterialThumbnailCacheKeyContract },
		{ "ShaderPropertyParser", &HEngine::Tests::ShaderPropertyParserContract },
		{ "MaterialSerialization", &HEngine::Tests::MaterialSerializationContract },
		{ "GroupedAssetOutputNames", &GroupedAssetOutputNames },
		{ "MeshResourceValidation", &MeshResourceValidation },
		{ "MeshCpuDecode", &HEngine::Tests::MeshCpuDecodeContract },
		{ "PluginSystem", &HEngine::Tests::PluginSystemContract },
		{ "PlatformProcessRelaunch", &PlatformProcessRelaunch },
		{ "ReflectedResourceSchemaCompatibility", &HEngine::Tests::ReflectedResourceSchemaCompatibilityContract },
		{ "RenderCapabilityPolicy", &HEngine::Tests::RenderCapabilityPolicyContract },
		{ "RenderGraphAccelerationStructure", &HEngine::Tests::RenderGraphAccelerationStructureContract },
		{ "RenderGraph", &HEngine::Tests::RenderGraphContract },
		{ "RenderGraphResourcePool", &HEngine::Tests::RenderGraphResourcePoolPureContract },
		{ "RenderSettings", &HEngine::Tests::RenderSettingsContract },
		{ "MaterialDescriptorPaging", &HEngine::Tests::MaterialDescriptorPagingContract },
		{ "TextureAlphaDilation", &HEngine::Tests::TextureAlphaDilationContract },
		{ "PipelineCacheKeys", &HEngine::Tests::PipelineCacheKeyContract },
		{ "ShaderCacheKey", &HEngine::Tests::ShaderCacheKeyContract },
		{ "TextureReadbackPng", &HEngine::Tests::TextureReadbackPngContract },
		{ "RenderWorldExtractorMeshPath", &HEngine::Tests::RenderWorldExtractorMeshPathContract },
		{ "RenderWorldSubmission", &HEngine::Tests::RenderWorldSubmissionContract },
		{ "ViewportRendererExtension", &HEngine::Tests::ViewportRendererExtensionContract },
		{ "RuntimePrefabResource", &HEngine::Tests::RuntimePrefabResourceContract },
		{ "RuntimeResourceCatalog", &HEngine::Tests::RuntimeResourceCatalogContract },
		{ "RuntimeResourceRegistryLifecycle", &HEngine::Tests::RuntimeResourceRegistryLifecycle },
		{ "RuntimeResourceWarmup", &HEngine::Tests::RuntimeResourceWarmupContract },
		{ "RuntimeSceneRequest", &HEngine::Tests::RuntimeSceneRequestContract },
		{ "RuntimeUiResource", &HEngine::Tests::RuntimeUiResourceContract },
		{ "RmlUiPointerPayload", &HEngine::Tests::RmlUiPointerPayloadContract },
		{ "SceneDocumentIncremental", &HEngine::Tests::SceneDocumentContract, true },
		{ "SystemLifecycle", &HEngine::Tests::SystemLifecycleContract },
		{ "PbrLightingReflection", &PbrLightingReflection, true },
		{ "EmbreeSubMeshPickResolve", &EmbreeSubMeshPickResolve },
		{ "FixedSimulationClock", &FixedSimulationClock },
		{ "PresentationClock", &PresentationClockContract },
		{ "ApplicationRuntimeStartupGuard", &ApplicationRuntimeStartupGuard }
	});
}

int main(int argc, char** argv)
{
	std::string_view activeTestName = "<startup>";
	try
	{
		if (argc == 3 &&
			std::string_view(argv[1]) ==
				"--platform-process-relaunch-child")
		{
			std::ofstream marker(
				std::filesystem::path(argv[2]),
				std::ios::binary | std::ios::trunc);
			marker << "HEngine process relaunch child";
			return marker ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		HEngine::Log::Init();
		if (argc == 2 && std::string_view(argv[1]) == "--list")
		{
			for (const TestCase& test : kTests)
				HE_CORE_INFO("[Test] name={0}", test.Name);
			return EXIT_SUCCESS;
		}

		if (argc != 3 || std::string_view(argv[1]) != "--run")
		{
			HE_CORE_ERROR("Usage: HEngineTests.exe --list | --run <name>");
			return 2;
		}

		const std::string_view requestedName = argv[2];
		const auto test = std::find_if(kTests.begin(), kTests.end(), [requestedName](const TestCase& candidate)
		{
			return candidate.Name == requestedName;
		});
		if (test == kTests.end())
		{
			HE_CORE_ERROR("[Test] UNKNOWN name={0}", requestedName);
			return 2;
		}

		activeTestName = test->Name;
		RunTest(*test);
		HE_CORE_INFO("[Test] PASS name={0}", test->Name);
		return EXIT_SUCCESS;
	}
	catch (const std::exception& exception)
	{
		const std::string message = "[Test] FAIL name=" + std::string(activeTestName);
		HEngine::Log::WriteFallbackError(message, exception.what());
		return EXIT_FAILURE;
	}
	catch (...)
	{
		const std::string message = "[Test] FAIL name=" + std::string(activeTestName);
		HEngine::Log::WriteFallbackError(message, "unexpected error");
		return EXIT_FAILURE;
	}
}
