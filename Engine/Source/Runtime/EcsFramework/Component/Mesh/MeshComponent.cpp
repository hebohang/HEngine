#include "Hepch.h"

#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"

#include "Runtime/Graphics/Material/MaterialResourceCache.h"

#include <stdexcept>

namespace HEngine
{
	void MeshComponent::ApplyMaterialOverrides()
	{
		if (!mMesh)
			return;
		if (mAuthoredMaterials.size() != mMesh->mMaterial.size())
			mAuthoredMaterials = mMesh->mMaterial;
		else
			mMesh->mMaterial = mAuthoredMaterials;
		if (MaterialOverrides.empty())
			return;
		if (MaterialOverrides.size() > mMesh->mMaterial.size())
		{
			throw std::out_of_range(
				"MeshComponent material override count exceeds the mesh material slot count.");
		}

		for (std::size_t index = 0; index < MaterialOverrides.size(); ++index)
		{
			const std::string& materialPath = MaterialOverrides[index];
			if (materialPath.empty())
				continue;
			mMesh->mMaterial[index] =
				MaterialResourceCache::GetInstance().Get(materialPath);
		}
	}
}
