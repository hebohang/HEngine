#include "Editor/Preview/MaterialThumbnailService.h"

#include "Editor/Manager/IconManager.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialShaderManifest.h"
#include "Runtime/Graphics/Material/MaterialShaderPath.h"
#include "Runtime/Graphics/Material/MaterialThumbnailCacheKey.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace HEngine
{
	namespace
	{
		constexpr const char* PreviewModelPath =
			"Assets/Models/MaterialPreview/MaterialBall.hmesh";
		constexpr const char* PreviewModelBinaryPath =
			"Assets/Models/MaterialPreview/MaterialBall.bin";
		constexpr const char* PreviewSurfaceSlotPath =
			"Assets/Models/MaterialPreview/DefaultMaterial_0.hmat";
		constexpr const char* PreviewNeutralSlotPath =
			"Assets/Models/MaterialPreview/shaderball.hmat";
		constexpr const char* PreviewEnvironmentPath =
			"Assets/Textures/Hdr/Default.hdr";
		constexpr const char* PreviewRigVersion = "material-thumbnail-rig-v1";
		// v2 stores display-encoded RGBA8 PNGs and registers them as UNORM in ImGui.
		constexpr uint32_t ThumbnailRendererVersion = 2;

		[[nodiscard]] std::string MakeQueueToken(
			std::string_view materialPath,
			std::string_view key)
		{
			return std::string(materialPath) + "\n" + std::string(key);
		}

		[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				throw std::runtime_error("Could not read material thumbnail input: " + path.string());
			return std::string(
				std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>());
		}

		[[nodiscard]] MaterialThumbnailFileIdentity BuildFileIdentity(
			std::string assetPath,
			uint64_t cacheEpoch)
		{
			assetPath = Utils::NormalizeAssetPath(assetPath);
			struct EpochCache
			{
				uint64_t Epoch = std::numeric_limits<uint64_t>::max();
				std::unordered_map<std::string, MaterialThumbnailFileIdentity> Identities;
			};
			thread_local EpochCache cache;
			if (cache.Epoch != cacheEpoch)
			{
				cache.Epoch = cacheEpoch;
				cache.Identities.clear();
			}
			if (const auto cached = cache.Identities.find(assetPath);
				cached != cache.Identities.end())
			{
				return cached->second;
			}

			MaterialThumbnailFileIdentity identity{ .AssetPath = assetPath };
			std::error_code error;
			const std::filesystem::path fullPath = ConfigManager::GetFullPath(assetPath);
			if (!std::filesystem::is_regular_file(fullPath, error) || error)
			{
				cache.Identities.emplace(assetPath, identity);
				return identity;
			}
			std::string hashError;
			if (!ComputeMaterialShaderFileSha256(fullPath, identity.ContentHash, hashError))
				throw std::runtime_error(hashError);
			cache.Identities.emplace(assetPath, identity);
			return identity;
		}

		[[nodiscard]] std::string FormatFileIdentity(
			const MaterialThumbnailFileIdentity& identity)
		{
			return identity.AssetPath + ":" + identity.ContentHash;
		}

		[[nodiscard]] bool IsBuiltinTexture(std::string_view path)
		{
			std::string lower(path);
			std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value)
			{
				return static_cast<char>(std::tolower(value));
			});
			return lower.empty() || lower == "white" || lower == "black" || lower == "normal";
		}

		[[nodiscard]] bool IsRegularFile(const std::filesystem::path& path) noexcept
		{
			std::error_code error;
			return std::filesystem::is_regular_file(path, error) && !error;
		}

		void AddTexturePath(
			std::vector<MaterialThumbnailFileIdentity>& textures,
			std::unordered_set<std::string>& seen,
			const Json& value,
			uint64_t cacheEpoch)
		{
			if (!value.is_string())
				return;
			const std::string requestedPath = value.get<std::string>();
			if (IsBuiltinTexture(requestedPath))
				return;

			std::string path;
			const Utils::PathValidationResult validation =
				Utils::TryNormalizeProjectRelativePath(requestedPath, path);
			if (!validation || !path.starts_with("Assets/") ||
				!IsValidMaterialShaderTextureDefault(path))
			{
				const std::string invalidIdentity = "invalid-texture-path:" + requestedPath;
				if (seen.insert(invalidIdentity).second)
					textures.push_back({ .AssetPath = invalidIdentity });
				return;
			}
			if (!seen.insert(path).second)
				return;
			textures.push_back(BuildFileIdentity(path, cacheEpoch));
		}

		void CollectTextureDependencies(
			const Json& document,
			const std::vector<std::string>& typedTextureNames,
			std::vector<MaterialThumbnailFileIdentity>& textures,
			uint64_t cacheEpoch)
		{
			std::unordered_set<std::string> seen;
			if (document.contains("Textures") && document["Textures"].is_object())
			{
				for (auto iterator = document["Textures"].begin(); iterator != document["Textures"].end(); ++iterator)
				{
					const Json& entry = iterator.value();
					if (!entry.is_object())
						continue;
					std::string type = entry.value("Type", std::string{});
					std::transform(type.begin(), type.end(), type.begin(), [](unsigned char value)
					{
						return static_cast<char>(std::tolower(value));
					});
					if (type != "texture2d")
						continue;
					if (entry.contains("Path"))
						AddTexturePath(textures, seen, entry["Path"], cacheEpoch);
				}
			}

			if (!document.contains("Properties") || !document["Properties"].is_object())
				return;
			const Json& properties = document["Properties"];
			const std::array<std::vector<std::string>, 5> legacyPaths = {{
				{ "pbrMetallicRoughness", "baseColorTexture", "textureRes", "textureSourcePath" },
				{ "pbrMetallicRoughness", "metallicRoughnessTexture", "textureRes", "textureSourcePath" },
				{ "normalTexture", "textureRes", "textureSourcePath" },
				{ "occlusionTexture", "textureRes", "textureSourcePath" },
				{ "emissiveTexture", "textureRes", "textureSourcePath" },
			}};
			for (const auto& components : legacyPaths)
			{
				const Json* value = &properties;
				for (const std::string& component : components)
				{
					if (!value->is_object() || !value->contains(component))
					{
						value = nullptr;
						break;
					}
					value = &(*value)[component];
				}
				if (value)
					AddTexturePath(textures, seen, *value, cacheEpoch);
			}

			if (!properties.contains("ExposedProperties") || !properties["ExposedProperties"].is_object())
				return;
			const Json& exposed = properties["ExposedProperties"];
			static constexpr const char* KnownTextureNames[] = {
				"g_AlbedoMap", "g_BaseColorMap", "g_NormalMap",
				"g_MetallicRoughnessMap", "g_AoMap", "g_EmissiveMap"
			};
			for (const char* name : KnownTextureNames)
			{
				if (exposed.contains(name))
					AddTexturePath(textures, seen, exposed[name], cacheEpoch);
			}
			for (const std::string& name : typedTextureNames)
			{
				if (exposed.contains(name))
					AddTexturePath(textures, seen, exposed[name], cacheEpoch);
			}
		}

		[[nodiscard]] std::string BuildShaderIdentity(
			const Json& document,
			uint32_t& materialAbiVersion,
			std::vector<std::string>& typedTextureNames,
			std::string& artifactIdentity,
			uint64_t cacheEpoch)
		{
			artifactIdentity.clear();
			const std::string requestedShaderPath = document.value(
				"ShaderPath", std::string("Shaders/BasePBR.hlsl"));
			std::string shaderPath;
			std::string pathError;
			std::filesystem::path manifestPath;
			if (!TryResolvePublishedMaterialShaderManifestPath(
					ConfigManager::GetInstance().GetEngineGeneratedContentFolder(),
					requestedShaderPath,
					manifestPath,
					shaderPath,
					pathError))
			{
				materialAbiVersion = MaterialShaderAbiVersion;
				artifactIdentity = "invalid-material-shader-path";
				return "invalid-material-shader-path|" + requestedShaderPath;
			}
			std::error_code error;
			if (std::filesystem::is_regular_file(manifestPath, error) && !error)
			{
				const Json manifest = Utils::LoadJsonFile(manifestPath);
				materialAbiVersion = manifest.value(
					"MaterialAbiVersion",
					MaterialShaderAbiVersion);
				if (manifest.contains("Properties") && manifest["Properties"].is_array())
				{
					for (const Json& property : manifest["Properties"])
					{
						if (property.is_object() && property.value("Type", std::string{}) == "Texture2D")
							typedTextureNames.push_back(property.value("Name", std::string{}));
					}
				}
				std::vector<std::string> artifactIdentities;
				if (manifest.contains("Artifacts") && manifest["Artifacts"].is_array())
				{
					artifactIdentities.reserve(manifest["Artifacts"].size());
					for (const Json& artifact : manifest["Artifacts"])
					{
						if (!artifact.is_object())
							continue;
						artifactIdentities.push_back(
							artifact.value("Pass", std::string{}) + "|" +
							artifact.value("VertexPath", std::string{}) + "|" +
							artifact.value("DescriptorMode", std::string{}) + "|" +
							artifact.value("Stage", std::string{}) + "|" +
							artifact.value("EntryPoint", std::string{}) + "|" +
							artifact.value("TargetProfile", std::string{}) + "|" +
							artifact.value("Permutation", std::string{}) + "|" +
							artifact.value("RelativePath", std::string{}) + "|" +
							artifact.value("ContentHash", std::string{}));
					}
				}
				std::sort(artifactIdentities.begin(), artifactIdentities.end());
				for (const std::string& identity : artifactIdentities)
				{
					artifactIdentity += identity;
					artifactIdentity.push_back('\n');
				}
				return shaderPath + "|" +
					manifest.value("RecursiveSourceHash", std::string{}) + "|" +
					manifest.value("BindingSignatureHash", std::string{});
			}

			materialAbiVersion = MaterialShaderAbiVersion;
			const std::string sourceIdentity = FormatFileIdentity(
				BuildFileIdentity(shaderPath, cacheEpoch));
			artifactIdentity = "source-fallback|" + sourceIdentity;
			return sourceIdentity;
		}
	}

	MaterialThumbnailService::MaterialThumbnailService()
		: mPreviewScene("Editor Material Thumbnail Renderer", {})
		, mCacheDirectory(ConfigManager::GetSavedPath("Editor/Thumbnails/Materials"))
	{
		std::error_code error;
		std::filesystem::create_directories(mCacheDirectory, error);
		if (error)
			throw std::runtime_error("Could not create material thumbnail cache: " + error.message());
	}

	MaterialThumbnailService::~MaterialThumbnailService()
	{
		Shutdown();
	}

	void MaterialThumbnailService::SetDirectoryGeneration(uint64_t generation)
	{
		if (generation == 0)
			generation = 1;
		if (generation == mDirectoryGeneration)
			return;
		mDirectoryGeneration = generation;
		mKeyQueue.clear();
		mKeyLookup.clear();
		mRenderQueue.clear();
		mRenderLookup.clear();
		mLoadQueue.clear();
		mLoadLookup.clear();
	}

	void MaterialThumbnailService::Request(std::string materialPath, uint64_t generation)
	{
		SetDirectoryGeneration(generation);
		materialPath = Utils::NormalizeAssetPath(materialPath);
		if (materialPath.empty())
			return;

		const uint64_t frameNumber = Application::GetInstance().GetFrameNumber();
		auto recordIterator = mRecords.find(materialPath);
		if (recordIterator != mRecords.end())
			recordIterator->second.LastRequestedFrame = frameNumber;
		const bool bNeedsEvaluation = recordIterator == mRecords.end() ||
			recordIterator->second.Key.empty() ||
			frameNumber < recordIterator->second.LastEvaluationFrame ||
			frameNumber - recordIterator->second.LastEvaluationFrame >= ReevaluateIntervalFrames;
		if (bNeedsEvaluation)
		{
			if (mKeyLookup.insert(materialPath).second)
				mKeyQueue.push_back({ materialPath, {}, generation, frameNumber });
			if (recordIterator == mRecords.end())
				return;
		}

		CacheRecord& record = mRecords[materialPath];
		record.Generation = generation;
		if (record.bFailed)
			return;
		if (IsRegularFile(record.CachePath))
		{
			TouchCacheFile(record, frameNumber);
			QueueLoad(materialPath, record);
		}
		else
			QueueRender(materialPath, record);
	}

	void MaterialThumbnailService::Invalidate(std::string_view materialPath)
	{
		mRecords.erase(Utils::NormalizeAssetPath(std::string(materialPath)));
	}

	ImGuiTextureHandle MaterialThumbnailService::TryGet(std::string_view materialPath)
	{
		const std::string normalized = Utils::NormalizeAssetPath(std::string(materialPath));
		const auto record = mRecords.find(normalized);
		if (record == mRecords.end() || record->second.CachePath.empty())
			return {};
		return IconManager::GetInstance().TryGetThumbnail(
			record->second.CachePath.string(),
			ThumbnailSize,
			false);
	}

	void MaterialThumbnailService::Tick(Timestep ts)
	{
		(void)ts;
		ProcessOneKeyEvaluation();
		ProcessOneLoad();
		if (mActiveJob)
			AdvanceActiveJob();
		if (!mActiveJob && StartOneJob())
			AdvanceActiveJob();
		RunCacheMaintenance(Application::GetInstance().GetFrameNumber());
	}

	void MaterialThumbnailService::Shutdown() noexcept
	{
		if (mActiveJob && mActiveJob->Capture)
		{
			try
			{
				Application& application = Application::GetInstance();
				if (application.IsOffscreenCaptureValid(mActiveJob->Capture))
					application.ReleaseOffscreenCapture(mActiveJob->Capture);
			}
			catch (...)
			{
			}
		}
		mPreviewScene.Release();
		mActiveJob.reset();
		mKeyQueue.clear();
		mKeyLookup.clear();
		mRenderQueue.clear();
		mRenderLookup.clear();
		mLoadQueue.clear();
		mLoadLookup.clear();
		mRecords.clear();
	}

	MaterialThumbnailService::CacheRecord MaterialThumbnailService::BuildRecord(
		const std::string& materialPath,
		uint64_t generation,
		uint64_t frameNumber) const
	{
		const std::filesystem::path fullPath = ConfigManager::GetFullPath(materialPath);
		const std::string materialContent = ReadTextFile(fullPath);
		const Json document = Json::parse(materialContent);
		const uint64_t dependencyCacheEpoch =
			frameNumber / ReevaluateIntervalFrames;

		MaterialThumbnailCacheKeyDesc keyDesc;
		keyDesc.MaterialDocumentContent = materialContent;
		std::vector<std::string> typedTextureNames;
		keyDesc.ShaderIdentity = BuildShaderIdentity(
			document,
			keyDesc.MaterialAbiVersion,
			typedTextureNames,
			keyDesc.ShaderArtifactIdentity,
			dependencyCacheEpoch);
		CollectTextureDependencies(
			document,
			typedTextureNames,
			keyDesc.TextureDependencies,
			dependencyCacheEpoch);
		keyDesc.PreviewModelIdentity =
			FormatFileIdentity(BuildFileIdentity(PreviewModelPath, dependencyCacheEpoch)) + "|" +
			FormatFileIdentity(BuildFileIdentity(PreviewModelBinaryPath, dependencyCacheEpoch)) + "|" +
			FormatFileIdentity(BuildFileIdentity(PreviewSurfaceSlotPath, dependencyCacheEpoch)) + "|" +
			FormatFileIdentity(BuildFileIdentity(PreviewNeutralSlotPath, dependencyCacheEpoch));
		keyDesc.EnvironmentIdentity = FormatFileIdentity(
			BuildFileIdentity(PreviewEnvironmentPath, dependencyCacheEpoch));
		keyDesc.RigVersion = PreviewRigVersion;
		keyDesc.RendererVersion = ThumbnailRendererVersion;

		CacheRecord record;
		record.Key = BuildMaterialThumbnailCacheKey(keyDesc);
		record.CachePath = mCacheDirectory / (record.Key + ".png");
		record.LastEvaluationFrame = frameNumber;
		record.Generation = generation;
		return record;
	}

	void MaterialThumbnailService::QueueRender(
		const std::string& materialPath,
		const CacheRecord& record)
	{
		const std::string token = MakeQueueToken(materialPath, record.Key);
		if (mActiveJob && MakeQueueToken(
			mActiveJob->Job.MaterialPath,
			mActiveJob->Job.Key) == token)
		{
			return;
		}
		if (!mRenderLookup.insert(token).second)
			return;
		mRenderQueue.push_back({
			materialPath, record.Key, record.Generation, record.LastRequestedFrame });
	}

	void MaterialThumbnailService::QueueLoad(
		const std::string& materialPath,
		const CacheRecord& record)
	{
		if (IconManager::GetInstance().TryGetThumbnail(
			record.CachePath.string(), ThumbnailSize, false))
		{
			return;
		}
		const std::string token = MakeQueueToken(materialPath, record.Key);
		if (!mLoadLookup.insert(token).second)
			return;
		mLoadQueue.push_back({
			materialPath, record.Key, record.Generation, record.LastRequestedFrame });
	}

	void MaterialThumbnailService::ProcessOneKeyEvaluation()
	{
		while (!mKeyQueue.empty())
		{
			QueuedJob job = std::move(mKeyQueue.front());
			mKeyQueue.pop_front();
			mKeyLookup.erase(job.MaterialPath);
			if (job.Generation != mDirectoryGeneration)
				continue;

			const uint64_t frameNumber = Application::GetInstance().GetFrameNumber();
			try
			{
				CacheRecord evaluated = BuildRecord(
					job.MaterialPath,
					job.Generation,
					frameNumber);
				evaluated.LastRequestedFrame = job.RequestedFrame;
				CacheRecord& record = mRecords[job.MaterialPath];
				if (record.Key != evaluated.Key)
					record = std::move(evaluated);
				else
				{
					record.LastEvaluationFrame = frameNumber;
					record.LastRequestedFrame = std::max(
						record.LastRequestedFrame,
						job.RequestedFrame);
					record.Generation = job.Generation;
					record.bFailed = false;
				}

				if (record.bFailed)
					return;
				if (IsRegularFile(record.CachePath))
				{
					TouchCacheFile(record, frameNumber);
					QueueLoad(job.MaterialPath, record);
				}
				else
					QueueRender(job.MaterialPath, record);
			}
			catch (const std::exception& exception)
			{
				HE_CORE_WARN("Material thumbnail key failed for '{}': {}",
					job.MaterialPath, exception.what());
				CacheRecord& failed = mRecords[job.MaterialPath];
				failed.LastEvaluationFrame = frameNumber;
				failed.Generation = job.Generation;
				failed.bFailed = true;
			}
			TrimMemoryRecords();
			return;
		}
	}

	void MaterialThumbnailService::ProcessOneLoad()
	{
		while (!mLoadQueue.empty())
		{
			QueuedJob job = std::move(mLoadQueue.front());
			mLoadQueue.pop_front();
			mLoadLookup.erase(MakeQueueToken(job.MaterialPath, job.Key));
			const auto record = mRecords.find(job.MaterialPath);
			const uint64_t frameNumber = Application::GetInstance().GetFrameNumber();
			if (job.Generation != mDirectoryGeneration || record == mRecords.end() ||
				record->second.Key != job.Key ||
				frameNumber > record->second.LastRequestedFrame + 2 ||
				!IsRegularFile(record->second.CachePath))
			{
				continue;
			}

			const ImGuiTextureHandle loaded = IconManager::GetInstance().LoadOrFindThumbnail(
				record->second.CachePath.string(), ThumbnailSize, false);
			if (loaded == IconManager::GetInstance().GetFileIcon())
			{
				std::error_code removeError;
				std::filesystem::remove(record->second.CachePath, removeError);
				record->second.bFailed = false;
				QueueRender(job.MaterialPath, record->second);
			}
			else
			{
				TouchCacheFile(record->second, frameNumber);
			}
			return;
		}
	}

	bool MaterialThumbnailService::StartOneJob()
	{
		while (!mRenderQueue.empty())
		{
			QueuedJob job = std::move(mRenderQueue.front());
			mRenderQueue.pop_front();
			mRenderLookup.erase(MakeQueueToken(job.MaterialPath, job.Key));
			const auto record = mRecords.find(job.MaterialPath);
			const uint64_t frameNumber = Application::GetInstance().GetFrameNumber();
			if (job.Generation != mDirectoryGeneration || record == mRecords.end() ||
				record->second.Key != job.Key || record->second.bFailed ||
				frameNumber > record->second.LastRequestedFrame + 2)
			{
				continue;
			}
			if (IsRegularFile(record->second.CachePath))
			{
				TouchCacheFile(record->second, frameNumber);
				QueueLoad(job.MaterialPath, record->second);
				continue;
			}

			try
			{
				Ref<Material> material = CreateRef<Material>(job.MaterialPath);
				if (material->mType == MaterialType::PostProcess ||
					(!material->mMatRes.Domain.empty() && material->mMatRes.Domain != "Surface"))
				{
					throw std::runtime_error("Material thumbnail supports only the Surface domain");
				}
				mPreviewScene.SetMaterial(material);
				mPreviewScene.ResetCamera();
				mActiveJob = ActiveJob{
					.Job = std::move(job),
					.MaterialDraft = std::move(material),
				};
				return true;
			}
			catch (const std::exception& exception)
			{
				record->second.bFailed = true;
				HE_CORE_WARN("Material thumbnail setup failed for '{}': {}",
					job.MaterialPath, exception.what());
				return false;
			}
		}
		return false;
	}

	void MaterialThumbnailService::AdvanceActiveJob()
	{
		if (!mActiveJob)
			return;
		ActiveJob& active = *mActiveJob;
		if (active.Capture)
		{
			Application& application = Application::GetInstance();
			if (!application.IsOffscreenCaptureValid(active.Capture))
			{
				FailActiveJob("offscreen capture was invalidated");
				return;
			}
			const TextureReadbackResult result =
				application.GetOffscreenCaptureResult(active.Capture);
			if (result.Status == TextureReadbackStatus::PendingSubmission ||
				result.Status == TextureReadbackStatus::InFlight)
			{
				return;
			}

			const bool bComplete = result.Status == TextureReadbackStatus::Complete;
			const std::string error = result.Error;
			application.ReleaseOffscreenCapture(active.Capture);
			const QueuedJob completedJob = active.Job;
			mActiveJob.reset();
			const auto record = mRecords.find(completedJob.MaterialPath);
			if (!bComplete)
			{
				if (record != mRecords.end() && record->second.Key == completedJob.Key)
					record->second.bFailed = true;
				HE_CORE_WARN("Material thumbnail capture failed for '{}': {}",
					completedJob.MaterialPath, error);
				return;
			}
			if (record != mRecords.end() && record->second.Key == completedJob.Key)
			{
				TouchCacheFile(
					record->second,
					Application::GetInstance().GetFrameNumber());
				QueueLoad(completedJob.MaterialPath, record->second);
			}
			return;
		}

		try
		{
			if (!mPreviewScene.Render(
				Timestep(0.0f),
				ThumbnailSize,
				ThumbnailSize,
				1.0f))
			{
				FailActiveJob(mPreviewScene.GetError());
				return;
			}
			if (active.WarmupFramesRemaining > 0)
				--active.WarmupFramesRemaining;
			if (active.WarmupFramesRemaining == 0)
			{
				const auto record = mRecords.find(active.Job.MaterialPath);
				if (record == mRecords.end() || record->second.Key != active.Job.Key)
				{
					mActiveJob.reset();
					return;
				}
				active.Capture = mPreviewScene.RequestCapture(record->second.CachePath);
			}
		}
		catch (const std::exception& exception)
		{
			FailActiveJob(exception.what());
		}
	}

	void MaterialThumbnailService::RunCacheMaintenance(uint64_t frameNumber)
	{
		if (mHasRunCacheMaintenance && frameNumber >= mLastCacheMaintenanceFrame &&
			frameNumber - mLastCacheMaintenanceFrame < CacheMaintenanceIntervalFrames)
		{
			return;
		}
		mHasRunCacheMaintenance = true;
		mLastCacheMaintenanceFrame = frameNumber;
		TrimMemoryRecords();
		TrimDiskCache();
	}

	void MaterialThumbnailService::TrimMemoryRecords()
	{
		if (mRecords.size() <= MaximumMemoryRecords)
			return;

		std::unordered_set<std::string> protectedPaths = mKeyLookup;
		for (const QueuedJob& job : mRenderQueue)
			protectedPaths.insert(job.MaterialPath);
		for (const QueuedJob& job : mLoadQueue)
			protectedPaths.insert(job.MaterialPath);
		if (mActiveJob)
			protectedPaths.insert(mActiveJob->Job.MaterialPath);

		std::vector<MaterialThumbnailCacheEntry> entries;
		entries.reserve(mRecords.size());
		for (const auto& [materialPath, record] : mRecords)
		{
			entries.push_back({
				.Key = materialPath,
				.ByteSize = 0,
				.LastAccessOrder = static_cast<std::int64_t>(std::min<uint64_t>(
					record.LastRequestedFrame,
					static_cast<uint64_t>(std::numeric_limits<std::int64_t>::max()))),
				.bProtected = protectedPaths.contains(materialPath),
			});
		}
		for (const std::string& materialPath : SelectMaterialThumbnailCacheEvictions(
				std::move(entries),
				MaximumMemoryRecords,
				std::numeric_limits<uint64_t>::max()))
		{
			mRecords.erase(materialPath);
		}
	}

	void MaterialThumbnailService::TrimDiskCache()
	{
		std::unordered_set<std::string> protectedKeys;
		auto protectRecord = [this, &protectedKeys](std::string_view materialPath)
		{
			const auto record = mRecords.find(std::string(materialPath));
			if (record != mRecords.end() && !record->second.Key.empty())
				protectedKeys.insert(record->second.Key);
		};
		for (const QueuedJob& job : mKeyQueue)
			protectRecord(job.MaterialPath);
		for (const QueuedJob& job : mRenderQueue)
			protectedKeys.insert(job.Key);
		for (const QueuedJob& job : mLoadQueue)
			protectedKeys.insert(job.Key);
		if (mActiveJob)
			protectedKeys.insert(mActiveJob->Job.Key);

		std::vector<MaterialThumbnailCacheEntry> entries;
		std::error_code error;
		std::filesystem::directory_iterator iterator(mCacheDirectory, error);
		const std::filesystem::directory_iterator end;
		while (!error && iterator != end)
		{
			const std::filesystem::directory_entry entry = *iterator;
			iterator.increment(error);
			std::error_code entryError;
			if (!entry.is_regular_file(entryError) || entryError ||
				entry.path().extension() != ".png")
			{
				continue;
			}
			const std::string key = entry.path().stem().string();
			if (key.size() != 64 || !std::all_of(key.begin(), key.end(), [](unsigned char value)
			{
				return std::isdigit(value) != 0 || (value >= 'a' && value <= 'f');
			}))
			{
				continue;
			}

			const uint64_t byteSize = entry.file_size(entryError);
			if (entryError)
				continue;
			const auto writeTime = entry.last_write_time(entryError);
			if (entryError)
				continue;
			entries.push_back({
				.Key = key,
				.ByteSize = byteSize,
				.LastAccessOrder = static_cast<std::int64_t>(
					writeTime.time_since_epoch().count()),
				.bProtected = protectedKeys.contains(key),
			});
		}

		for (const std::string& key : SelectMaterialThumbnailCacheEvictions(
				std::move(entries),
				MaximumDiskEntries,
				MaximumDiskBytes))
		{
			std::error_code removeError;
			std::filesystem::remove(mCacheDirectory / (key + ".png"), removeError);
		}
	}

	void MaterialThumbnailService::TouchCacheFile(
		CacheRecord& record,
		uint64_t frameNumber)
	{
		if (record.LastDiskTouchFrame != 0 &&
			frameNumber >= record.LastDiskTouchFrame &&
			frameNumber - record.LastDiskTouchFrame < DiskTouchIntervalFrames)
		{
			return;
		}
		record.LastDiskTouchFrame = frameNumber;
		std::error_code error;
		if (std::filesystem::is_regular_file(record.CachePath, error) && !error)
		{
			std::filesystem::last_write_time(
				record.CachePath,
				std::filesystem::file_time_type::clock::now(),
				error);
		}
	}

	void MaterialThumbnailService::FailActiveJob(std::string_view error)
	{
		if (!mActiveJob)
			return;
		const QueuedJob failedJob = mActiveJob->Job;
		mActiveJob.reset();
		mPreviewScene.Release();
		const auto record = mRecords.find(failedJob.MaterialPath);
		if (record != mRecords.end() && record->second.Key == failedJob.Key)
			record->second.bFailed = true;
		HE_CORE_WARN("Material thumbnail generation failed for '{}': {}",
			failedJob.MaterialPath, error);
	}
}
