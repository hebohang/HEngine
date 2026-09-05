#pragma once

#include "Editor/Preview/SceneThumbnailPreview.h"

#include "Runtime/Core/Timestep.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace HEngine
{
	class SceneDocument;

	class SceneThumbnailService final
	{
	public:
		SceneThumbnailService();
		~SceneThumbnailService();

		SceneThumbnailService(const SceneThumbnailService&) = delete;
		SceneThumbnailService& operator=(const SceneThumbnailService&) = delete;

		void SetDirectoryGeneration(uint64_t generation);
		void Request(std::string scenePath, uint64_t generation);
		void Invalidate(std::string_view scenePath);
		[[nodiscard]] ImGuiTextureHandle TryGet(std::string_view scenePath);
		void Tick(Timestep ts);
		void Shutdown() noexcept;

	private:
		struct CacheRecord
		{
			std::string Key;
			std::filesystem::path CachePath;
			uint64_t LastEvaluationFrame = 0;
			uint64_t LastRequestedFrame = 0;
			uint64_t LastDiskTouchFrame = 0;
			uint64_t Generation = 0;
			bool bFailed = false;
		};

		struct QueuedJob
		{
			std::string ScenePath;
			std::string Key;
			uint64_t Generation = 0;
		};

		enum class ActiveStage : uint8_t
		{
			Parsing,
			Materializing,
			Rendering,
			Capturing,
		};

		struct ActiveJob
		{
			QueuedJob Job;
			ActiveStage Stage = ActiveStage::Parsing;
			std::stop_source ParseStop;
			std::future<std::shared_ptr<const SceneDocument>> ParseFuture;
			TextureReadbackHandle Capture;
			uint32_t WarmupFramesRemaining = 3;
		};

		[[nodiscard]] CacheRecord BuildRecord(
			const std::string& scenePath,
			uint64_t generation,
			uint64_t frameNumber) const;
		void QueueRender(const std::string& scenePath, const CacheRecord& record);
		void QueueLoad(const std::string& scenePath, const CacheRecord& record);
		void ProcessOneLoad();
		[[nodiscard]] bool StartOneJob();
		void AdvanceActiveJob(Timestep ts);
		[[nodiscard]] bool IsActiveJobCurrent() const;
		void FinishActiveJob();
		void CancelActiveJob() noexcept;
		void FailActiveJob(std::string_view error);
		void RunCacheMaintenance(uint64_t frameNumber);
		void TrimMemoryRecords();
		void TrimDiskCache();
		void TouchCacheFile(CacheRecord& record, uint64_t frameNumber);

		static constexpr uint32_t ThumbnailSize = 128;
		static constexpr uint64_t ReevaluateIntervalFrames = 60;
		static constexpr uint64_t CacheMaintenanceIntervalFrames = 300;
		static constexpr uint64_t DiskTouchIntervalFrames = 1800;
		static constexpr std::size_t MaximumMemoryRecords = 256;
		static constexpr std::size_t MaximumDiskEntries = 256;
		static constexpr uint64_t MaximumDiskBytes = 256ull * 1024ull * 1024ull;

		SceneThumbnailPreview mPreview;
		IncrementalSceneMaterializer mMaterializer;
		std::filesystem::path mCacheDirectory;
		std::unordered_map<std::string, CacheRecord> mRecords;
		std::deque<QueuedJob> mRenderQueue;
		std::unordered_set<std::string> mRenderLookup;
		std::deque<QueuedJob> mLoadQueue;
		std::unordered_set<std::string> mLoadLookup;
		std::optional<ActiveJob> mActiveJob;
		uint64_t mDirectoryGeneration = 1;
		uint64_t mLastCacheMaintenanceFrame = 0;
		bool mHasRunCacheMaintenance = false;
	};
}
