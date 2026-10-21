#include "Editor/Preview/SceneThumbnailService.h"

#include "Editor/Manager/IconManager.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/Graphics/Material/MaterialShaderManifest.h"
#include "Runtime/Graphics/Material/MaterialThumbnailCacheKey.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace HEngine
{
	namespace
	{
		constexpr uint32_t ThumbnailRendererVersion = 2;

		[[nodiscard]] std::string MakeQueueToken(
			std::string_view scenePath,
			std::string_view key)
		{
			return std::string(scenePath) + "\n" + std::string(key);
		}

		[[nodiscard]] bool IsRegularFile(const std::filesystem::path& path) noexcept
		{
			std::error_code error;
			return std::filesystem::is_regular_file(path, error) && !error;
		}

		[[nodiscard]] bool ShouldUse3D(const SceneDocument& document)
		{
			bool bHas2DRenderable = false;
			for (const SceneDocumentEntity& entity : document.GetEntities())
			{
				if (entity.Value.contains("MeshComponent") ||
					entity.Value.contains("DirectionalLightComponent") ||
					entity.Value.contains("PointLightComponent") ||
					entity.Value.contains("EnvironmentLightComponent") ||
					entity.Value.contains("Rigidbody3DComponent"))
				{
					return true;
				}
				bHas2DRenderable |= entity.Value.contains("SpriteRendererComponent") ||
					entity.Value.contains("CircleRendererComponent");
			}
			return bHas2DRenderable ? false : ModeManager::b3DMode != 0;
		}

		void AppendNestedExceptionMessages(
			const std::exception& exception,
			std::string& messages)
		{
			if (!messages.empty())
				messages += " | ";
			messages += exception.what();
			try
			{
				std::rethrow_if_nested(exception);
			}
			catch (const std::exception& nested)
			{
				AppendNestedExceptionMessages(nested, messages);
			}
			catch (...)
			{
				messages += " | <non-standard exception>";
			}
		}

		[[nodiscard]] std::string DescribeException(const std::exception& exception)
		{
			std::string messages;
			AppendNestedExceptionMessages(exception, messages);
			return messages;
		}

		class ScopedDimensionMode final
		{
		public:
			explicit ScopedDimensionMode(bool bUse3D)
				: mPrevious(ModeManager::b3DMode)
			{
				ModeManager::b3DMode = bUse3D ? 1 : 0;
			}

			~ScopedDimensionMode() noexcept
			{
				ModeManager::b3DMode = mPrevious;
			}

		private:
			int mPrevious = 1;
		};
	}

	SceneThumbnailService::SceneThumbnailService()
		: mPreview("Editor Scene Thumbnail Renderer"),
		  mCacheDirectory(ConfigManager::GetSavedPath("Editor/Thumbnails/Scenes"))
	{
		std::error_code error;
		std::filesystem::create_directories(mCacheDirectory, error);
		if (error)
			throw std::runtime_error("Could not create scene thumbnail cache: " + error.message());
	}

	SceneThumbnailService::~SceneThumbnailService()
	{
		Shutdown();
	}

	void SceneThumbnailService::SetDirectoryGeneration(uint64_t generation)
	{
		if (generation == 0)
			generation = 1;
		if (generation == mDirectoryGeneration)
			return;
		mDirectoryGeneration = generation;
		mRenderQueue.clear();
		mRenderLookup.clear();
		mLoadQueue.clear();
		mLoadLookup.clear();
		CancelActiveJob();
	}

	void SceneThumbnailService::Request(std::string scenePath, uint64_t generation)
	{
		SetDirectoryGeneration(generation);
		scenePath = Utils::NormalizeAssetPath(scenePath);
		if (scenePath.empty())
			return;

		const uint64_t frameNumber = Application::GetInstance().GetFrameNumber();
		auto recordIterator = mRecords.find(scenePath);
		const bool bNeedsEvaluation = recordIterator == mRecords.end() ||
			recordIterator->second.Key.empty() ||
			frameNumber < recordIterator->second.LastEvaluationFrame ||
			frameNumber - recordIterator->second.LastEvaluationFrame >= ReevaluateIntervalFrames;
		if (bNeedsEvaluation)
		{
			try
			{
				CacheRecord evaluated = BuildRecord(scenePath, generation, frameNumber);
				CacheRecord& record = mRecords[scenePath];
				if (record.Key != evaluated.Key)
					record = std::move(evaluated);
				else
				{
					record.LastEvaluationFrame = frameNumber;
					record.Generation = generation;
				}
				recordIterator = mRecords.find(scenePath);
			}
			catch (const std::exception& exception)
			{
				HE_CORE_WARN("Scene thumbnail key failed for '{}': {}", scenePath, exception.what());
				CacheRecord& failed = mRecords[scenePath];
				failed.LastEvaluationFrame = frameNumber;
				failed.Generation = generation;
				failed.bFailed = true;
				return;
			}
		}

		CacheRecord& record = recordIterator->second;
		record.LastRequestedFrame = frameNumber;
		record.Generation = generation;
		if (record.bFailed)
			return;
		if (IsRegularFile(record.CachePath))
		{
			TouchCacheFile(record, frameNumber);
			QueueLoad(scenePath, record);
		}
		else
		{
			QueueRender(scenePath, record);
		}
		TrimMemoryRecords();
	}

	void SceneThumbnailService::Invalidate(std::string_view scenePath)
	{
		const std::string normalized = Utils::NormalizeAssetPath(std::string(scenePath));
		if (mActiveJob && mActiveJob->Job.ScenePath == normalized)
			CancelActiveJob();
		mRecords.erase(normalized);
	}

	ImGuiTextureHandle SceneThumbnailService::TryGet(std::string_view scenePath)
	{
		const std::string normalized = Utils::NormalizeAssetPath(std::string(scenePath));
		const auto record = mRecords.find(normalized);
		if (record == mRecords.end() || record->second.CachePath.empty())
			return {};
		return IconManager::GetInstance().TryGetThumbnail(
			record->second.CachePath.string(),
			ThumbnailSize,
			false);
	}

	void SceneThumbnailService::Tick(Timestep ts)
	{
		ProcessOneLoad();
		if (mActiveJob)
			AdvanceActiveJob(ts);
		if (!mActiveJob && StartOneJob())
			AdvanceActiveJob(ts);
		RunCacheMaintenance(Application::GetInstance().GetFrameNumber());
	}

	void SceneThumbnailService::Shutdown() noexcept
	{
		CancelActiveJob();
		mPreview.Release();
		mRenderQueue.clear();
		mRenderLookup.clear();
		mLoadQueue.clear();
		mLoadLookup.clear();
		mRecords.clear();
	}

	SceneThumbnailService::CacheRecord SceneThumbnailService::BuildRecord(
		const std::string& scenePath,
		uint64_t generation,
		uint64_t frameNumber) const
	{
		const std::filesystem::path fullPath = ConfigManager::GetFullPath(scenePath);
		std::error_code error;
		const uint64_t fileSize = std::filesystem::file_size(fullPath, error);
		if (error)
			throw std::runtime_error("Could not stat scene thumbnail input: " + error.message());
		const auto writeTime = std::filesystem::last_write_time(fullPath, error);
		if (error)
			throw std::runtime_error("Could not read scene thumbnail timestamp: " + error.message());

		std::ostringstream identity;
		const auto elapsed = writeTime.time_since_epoch();
		const auto wholeSeconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
		const auto fractionalNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed - wholeSeconds);
		identity << scenePath << '\n'
			<< fileSize << '\n'
			<< wholeSeconds.count() << ':' << fractionalNanoseconds.count() << '\n'
			<< ThumbnailRendererVersion;
		const std::string identityText = identity.str();
		const std::string key = ComputeMaterialShaderSha256(std::as_bytes(
			std::span(identityText.data(), identityText.size())));

		CacheRecord record;
		record.Key = key;
		record.CachePath = mCacheDirectory / (key + ".png");
		record.LastEvaluationFrame = frameNumber;
		record.Generation = generation;
		return record;
	}

	void SceneThumbnailService::QueueRender(
		const std::string& scenePath,
		const CacheRecord& record)
	{
		const std::string token = MakeQueueToken(scenePath, record.Key);
		if (mActiveJob && MakeQueueToken(
			mActiveJob->Job.ScenePath,
			mActiveJob->Job.Key) == token)
		{
			return;
		}
		if (!mRenderLookup.insert(token).second)
			return;
		mRenderQueue.push_back({ scenePath, record.Key, record.Generation });
	}

	void SceneThumbnailService::QueueLoad(
		const std::string& scenePath,
		const CacheRecord& record)
	{
		if (IconManager::GetInstance().TryGetThumbnail(
			record.CachePath.string(), ThumbnailSize, false))
		{
			return;
		}
		const std::string token = MakeQueueToken(scenePath, record.Key);
		if (!mLoadLookup.insert(token).second)
			return;
		mLoadQueue.push_back({ scenePath, record.Key, record.Generation });
	}

	void SceneThumbnailService::ProcessOneLoad()
	{
		while (!mLoadQueue.empty())
		{
			QueuedJob job = std::move(mLoadQueue.front());
			mLoadQueue.pop_front();
			mLoadLookup.erase(MakeQueueToken(job.ScenePath, job.Key));
			const auto record = mRecords.find(job.ScenePath);
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
				QueueRender(job.ScenePath, record->second);
			}
			else
			{
				TouchCacheFile(record->second, frameNumber);
			}
			return;
		}
	}

	bool SceneThumbnailService::StartOneJob()
	{
		while (!mRenderQueue.empty())
		{
			QueuedJob job = std::move(mRenderQueue.front());
			mRenderQueue.pop_front();
			mRenderLookup.erase(MakeQueueToken(job.ScenePath, job.Key));
			const auto record = mRecords.find(job.ScenePath);
			const uint64_t frameNumber = Application::GetInstance().GetFrameNumber();
			if (job.Generation != mDirectoryGeneration || record == mRecords.end() ||
				record->second.Key != job.Key || record->second.bFailed ||
				frameNumber > record->second.LastRequestedFrame + 2)
			{
				continue;
			}
			if (IsRegularFile(record->second.CachePath))
			{
				QueueLoad(job.ScenePath, record->second);
				continue;
			}

			try
			{
				const SceneDocumentPath documentPath = ResolveSceneDocumentPath(
					ConfigManager::GetInstance().GetRootFolder(),
					job.ScenePath);
				ActiveJob active;
				HE_CORE_INFO("Scene thumbnail starting: {}", job.ScenePath);
				active.Job = std::move(job);
				const std::stop_token stopToken = active.ParseStop.get_token();
				active.ParseFuture = std::async(
					std::launch::async,
					[documentPath, stopToken]
					{
						return std::make_shared<const SceneDocument>(
							ParseSceneDocument(documentPath, {}, stopToken));
					});
				mActiveJob.emplace(std::move(active));
				return true;
			}
			catch (const std::exception& exception)
			{
				record->second.bFailed = true;
				HE_CORE_WARN("Scene thumbnail setup failed for '{}': {}",
					record->first, exception.what());
				return false;
			}
		}
		return false;
	}

	void SceneThumbnailService::AdvanceActiveJob(Timestep ts)
	{
		if (!mActiveJob)
			return;
		if (!IsActiveJobCurrent())
		{
			CancelActiveJob();
			return;
		}

		ActiveJob& active = *mActiveJob;
		try
		{
			switch (active.Stage)
			{
			case ActiveStage::Parsing:
			{
				if (active.ParseFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
					return;
				std::shared_ptr<const SceneDocument> document = active.ParseFuture.get();
				const ScopedDimensionMode dimensionMode(ShouldUse3D(*document));
				mMaterializer.Start(RuntimeResourceRegistry::GetInstance(), std::move(document));
				if (mMaterializer.GetProgress().State == SceneMaterializationState::Failed)
					mMaterializer.RethrowFailure();
				active.Stage = ActiveStage::Materializing;
				return;
			}
			case ActiveStage::Materializing:
			{
				const SceneMaterializationProgress progress = mMaterializer.Pump({
					.TimeBudget = std::chrono::milliseconds(2),
					.MaxEntities = 16,
				});
				if (progress.State == SceneMaterializationState::Failed)
					mMaterializer.RethrowFailure();
				if (progress.State != SceneMaterializationState::Ready)
					return;
				mPreview.SetScene(mMaterializer.TakeCompletedLevel());
				active.Stage = ActiveStage::Rendering;
				return;
			}
			case ActiveStage::Rendering:
			{
				if (!mPreview.Render(ts, ThumbnailSize, ThumbnailSize))
					throw std::runtime_error(mPreview.GetError());
				if (active.WarmupFramesRemaining > 0)
					--active.WarmupFramesRemaining;
				if (active.WarmupFramesRemaining != 0)
					return;
				const auto record = mRecords.find(active.Job.ScenePath);
				if (record == mRecords.end() || record->second.Key != active.Job.Key)
				{
					CancelActiveJob();
					return;
				}
				active.Capture = mPreview.RequestCapture(record->second.CachePath);
				active.Stage = ActiveStage::Capturing;
				return;
			}
			case ActiveStage::Capturing:
			{
				Application& application = Application::GetInstance();
				if (!application.IsOffscreenCaptureValid(active.Capture))
					throw std::runtime_error("offscreen capture was invalidated");
				const TextureReadbackResult result = application.GetOffscreenCaptureResult(active.Capture);
				if (result.Status == TextureReadbackStatus::PendingSubmission ||
					result.Status == TextureReadbackStatus::InFlight)
				{
					return;
				}
				const bool bComplete = result.Status == TextureReadbackStatus::Complete;
				const std::string error = result.Error;
				application.ReleaseOffscreenCapture(active.Capture);
				active.Capture = {};
				if (!bComplete)
					throw std::runtime_error(error);
				FinishActiveJob();
				return;
			}
			}
		}
		catch (const std::exception& exception)
		{
			FailActiveJob(DescribeException(exception));
		}
	}

	bool SceneThumbnailService::IsActiveJobCurrent() const
	{
		if (!mActiveJob || mActiveJob->Job.Generation != mDirectoryGeneration)
			return false;
		const auto record = mRecords.find(mActiveJob->Job.ScenePath);
		if (record == mRecords.end() || record->second.Key != mActiveJob->Job.Key)
			return false;
		const uint64_t frameNumber = Application::GetInstance().GetFrameNumber();
		return frameNumber <= record->second.LastRequestedFrame + 2;
	}

	void SceneThumbnailService::FinishActiveJob()
	{
		if (!mActiveJob)
			return;
		const QueuedJob completed = mActiveJob->Job;
		HE_CORE_INFO("Scene thumbnail completed: {}", completed.ScenePath);
		mPreview.ClearScene();
		mActiveJob.reset();
		const auto record = mRecords.find(completed.ScenePath);
		if (record == mRecords.end() || record->second.Key != completed.Key)
			return;
		TouchCacheFile(record->second, Application::GetInstance().GetFrameNumber());
		QueueLoad(completed.ScenePath, record->second);
	}

	void SceneThumbnailService::CancelActiveJob() noexcept
	{
		if (!mActiveJob)
			return;
		try
		{
			mActiveJob->ParseStop.request_stop();
			mMaterializer.RequestCancel();
			if (mActiveJob->Capture)
			{
				Application& application = Application::GetInstance();
				if (application.IsOffscreenCaptureValid(mActiveJob->Capture))
					application.ReleaseOffscreenCapture(mActiveJob->Capture);
			}
		}
		catch (...)
		{
		}
		mPreview.ClearScene();
		mActiveJob.reset();
	}

	void SceneThumbnailService::FailActiveJob(std::string_view error)
	{
		if (!mActiveJob)
			return;
		const QueuedJob failed = mActiveJob->Job;
		CancelActiveJob();
		const auto record = mRecords.find(failed.ScenePath);
		if (record != mRecords.end() && record->second.Key == failed.Key)
			record->second.bFailed = true;
		HE_CORE_WARN("Scene thumbnail generation failed for '{}': {}", failed.ScenePath, error);
	}

	void SceneThumbnailService::RunCacheMaintenance(uint64_t frameNumber)
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

	void SceneThumbnailService::TrimMemoryRecords()
	{
		if (mRecords.size() <= MaximumMemoryRecords)
			return;
		std::vector<MaterialThumbnailCacheEntry> entries;
		entries.reserve(mRecords.size());
		for (const auto& [scenePath, record] : mRecords)
		{
			entries.push_back({
				.Key = scenePath,
				.LastAccessOrder = static_cast<std::int64_t>(std::min<uint64_t>(
					record.LastRequestedFrame,
					static_cast<uint64_t>(std::numeric_limits<std::int64_t>::max()))),
				.bProtected = mActiveJob && mActiveJob->Job.ScenePath == scenePath,
			});
		}
		for (const std::string& scenePath : SelectMaterialThumbnailCacheEvictions(
			std::move(entries),
			MaximumMemoryRecords,
			std::numeric_limits<uint64_t>::max()))
		{
			mRecords.erase(scenePath);
		}
	}

	void SceneThumbnailService::TrimDiskCache()
	{
		std::vector<MaterialThumbnailCacheEntry> entries;
		std::error_code error;
		std::filesystem::directory_iterator iterator(mCacheDirectory, error);
		const std::filesystem::directory_iterator end;
		while (!error && iterator != end)
		{
			const std::filesystem::directory_entry entry = *iterator;
			iterator.increment(error);
			std::error_code entryError;
			if (!entry.is_regular_file(entryError) || entryError || entry.path().extension() != ".png")
				continue;
			const uint64_t byteSize = entry.file_size(entryError);
			if (entryError)
				continue;
			const auto writeTime = entry.last_write_time(entryError);
			if (entryError)
				continue;
			entries.push_back({
				.Key = entry.path().stem().string(),
				.ByteSize = byteSize,
				.LastAccessOrder = static_cast<std::int64_t>(writeTime.time_since_epoch().count()),
			});
		}
		for (const std::string& key : SelectMaterialThumbnailCacheEvictions(
			std::move(entries), MaximumDiskEntries, MaximumDiskBytes))
		{
			std::error_code removeError;
			std::filesystem::remove(mCacheDirectory / (key + ".png"), removeError);
		}
	}

	void SceneThumbnailService::TouchCacheFile(CacheRecord& record, uint64_t frameNumber)
	{
		if (record.LastDiskTouchFrame != 0 && frameNumber >= record.LastDiskTouchFrame &&
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
}
