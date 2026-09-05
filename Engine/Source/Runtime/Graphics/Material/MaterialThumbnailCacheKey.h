#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HEngine
{
	struct MaterialThumbnailFileIdentity
	{
		std::string AssetPath;
		std::string ContentHash;

		[[nodiscard]] bool operator==(const MaterialThumbnailFileIdentity&) const = default;
	};

	struct MaterialThumbnailCacheKeyDesc
	{
		std::string MaterialDocumentContent;
		std::string ShaderIdentity;
		std::string ShaderArtifactIdentity;
		std::vector<MaterialThumbnailFileIdentity> TextureDependencies;
		std::string PreviewModelIdentity;
		std::string EnvironmentIdentity;
		std::string RigVersion;
		std::uint32_t MaterialAbiVersion = 0;
		std::uint32_t RendererVersion = 1;

		[[nodiscard]] bool operator==(const MaterialThumbnailCacheKeyDesc&) const = default;
	};

	struct MaterialThumbnailCacheEntry
	{
		std::string Key;
		std::uint64_t ByteSize = 0;
		std::int64_t LastAccessOrder = 0;
		bool bProtected = false;

		[[nodiscard]] bool operator==(const MaterialThumbnailCacheEntry&) const = default;
	};

	// Returns the complete lowercase SHA-256 digest. Texture dependency order is
	// canonicalized by path and content identity before hashing.
	[[nodiscard]] std::string BuildMaterialThumbnailCacheKey(
		const MaterialThumbnailCacheKeyDesc& desc);

	// Selects the oldest unprotected entries until both limits are met. Protected
	// active/queued entries are never returned, even when they alone exceed a limit.
	[[nodiscard]] std::vector<std::string> SelectMaterialThumbnailCacheEvictions(
		std::vector<MaterialThumbnailCacheEntry> entries,
		std::size_t maximumEntryCount,
		std::uint64_t maximumByteSize);
}
