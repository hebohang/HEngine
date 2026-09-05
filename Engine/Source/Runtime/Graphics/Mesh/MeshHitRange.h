#pragma once

#include <cstdint>
#include <vector>

namespace HEngine
{
	struct MeshHitRange
	{
		std::uint32_t SubMeshIndex = 0;
		std::uint32_t MaterialIndex = 0;
		std::uint32_t FirstPrimID = 0;
		std::uint32_t PrimCount = 0;
	};

	[[nodiscard]] bool ResolveMeshHitRange(
		const std::vector<MeshHitRange>& hitRanges,
		std::uint32_t primitiveID,
		std::uint32_t& outSubMeshIndex,
		std::uint32_t& outMaterialIndex);
}
