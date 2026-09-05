#pragma once

#include "Editor/Preview/MaterialPreviewScene.h"

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Timestep.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace HEngine
{
	class Material;

	class MaterialThumbnailService final
	{
	public:
		MaterialThumbnailService();
		~MaterialThumbnailService();

		MaterialThumbnailService(const MaterialThumbnailService&) = delete;
		MaterialThumbnailService& operator=(const MaterialThumbnailService&) = delete;

		void SetDirectoryGeneration(uint64_t generation);
		void Request(std::string materialPath, uint64_t generation);
		void Invalidate(std::string_view materialPath);
		[[nodiscard]] ImGuiTextureHandle TryGet(std::string_view materialPath);
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
			std::string MaterialPath;
			std::string Key;
			uint64_t Generation = 0;
			uint64_t RequestedFrame = 0;
		};

		struct ActiveJob
		{
			QueuedJob Job;
			Ref<Material> MaterialDraft;
			TextureReadbackHandle Capture;
			uint32_t WarmupFramesRemaining = 3;
		};

		[[nodiscard]] CacheRecord BuildRecord(
			const std::string& materialPath,
			uint64_t generation,
			uint64_t frameNumber) const;
		void QueueRender(const std::string& materialPath, const CacheRecord& record);
		void QueueLoad(const std::string& materialPath, const CacheRecord& record);
		void ProcessOneKeyEvaluation();
		void ProcessOneLoad();
		[[nodiscard]] bool StartOneJob();
		void AdvanceActiveJob();
		void FailActiveJob(std::string_view error);
		void RunCacheMaintenance(uint64_t frameNumber);
		void TrimMemoryRecords();
		void TrimDiskCache();
		void TouchCacheFile(CacheRecord& record, uint64_t frameNumber);

		static constexpr uint32_t ThumbnailSize = 256;
		static constexpr uint64_t ReevaluateIntervalFrames = 30;
		static constexpr uint64_t CacheMaintenanceIntervalFrames = 300;
		static constexpr uint64_t DiskTouchIntervalFrames = 1800;
		static constexpr std::size_t MaximumMemoryRecords = 512;
		static constexpr std::size_t MaximumDiskEntries = 512;
		static constexpr uint64_t MaximumDiskBytes = 128ull * 1024ull * 1024ull;

		MaterialPreviewScene mPreviewScene;
		std::filesystem::path mCacheDirectory;
		std::unordered_map<std::string, CacheRecord> mRecords;
		std::deque<QueuedJob> mKeyQueue;
		std::unordered_set<std::string> mKeyLookup;
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
