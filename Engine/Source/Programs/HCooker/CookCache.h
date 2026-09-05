#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine::Cooker
{
	inline constexpr std::uint32_t CookCacheSchemaVersion = 1;
	inline constexpr std::uint32_t CookCacheImplementationVersion = 1;
	inline constexpr std::string_view CookCacheHashAlgorithm = "sha256";

	struct CookCacheOption
	{
		std::string Name;
		std::string Value;
	};

	struct CookCacheKeyInput
	{
		std::string_view LogicalPath;
		std::span<const std::uint8_t> SourceBytes;
		std::span<const CookCacheOption> PayloadOptions;
	};

	struct CookCacheArtifact
	{
		std::vector<std::uint8_t> Payload;
	};

	struct CookCacheStats
	{
		std::uint64_t Cacheable = 0;
		std::uint64_t Hits = 0;
		std::uint64_t Misses = 0;
		std::uint64_t Stores = 0;
		std::uint64_t Corruptions = 0;
	};

	struct CookCacheEvent
	{
		std::string Path;
		std::string Key;
		std::string Outcome;
	};

	class CookCache
	{
	public:
		CookCache(std::filesystem::path root, std::uint64_t maxPayloadBytes);

		[[nodiscard]] std::string BuildKey(const CookCacheKeyInput& input) const;
		[[nodiscard]] std::optional<CookCacheArtifact> TryLoad(
			std::string_view key,
			std::string_view logicalPath);
		void Store(std::string_view key, std::span<const std::uint8_t> payload) noexcept;

		[[nodiscard]] const CookCacheStats& GetStats() const noexcept { return mStats; }
		[[nodiscard]] const std::vector<CookCacheEvent>& GetEvents() const noexcept { return mEvents; }

	private:
		std::filesystem::path mRoot;
		std::uint64_t mMaxPayloadBytes = 0;
		CookCacheStats mStats;
		std::vector<CookCacheEvent> mEvents;
	};
}
