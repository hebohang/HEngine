#include "Hepch.h"

#include "MeshHitRange.h"

namespace HEngine
{
	bool ResolveMeshHitRange(
		const std::vector<MeshHitRange>& hitRanges,
		std::uint32_t primitiveID,
		std::uint32_t& outSubMeshIndex,
		std::uint32_t& outMaterialIndex)
	{
		for (const MeshHitRange& hitRange : hitRanges)
		{
			if (primitiveID >= hitRange.FirstPrimID &&
				primitiveID - hitRange.FirstPrimID < hitRange.PrimCount)
			{
				outSubMeshIndex = hitRange.SubMeshIndex;
				outMaterialIndex = hitRange.MaterialIndex;
				return true;
			}
		}

		return false;
	}
}
