#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Animation/Animdata.h"
#include "Runtime/Animation/Animator.h"

#include "Runtime/Graphics/Mesh/MeshHitRange.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/Material/Material.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace HEngine
{
#if WITH_EDITOR
	struct MeshEditorPickData
	{
		std::vector<glm::vec3> Vertices;
		std::vector<std::uint32_t> TriangleIndices;
		std::vector<MeshHitRange> HitRanges;
	};

	struct MeshEditorPickDataCache
	{
		std::mutex Mutex;
		Ref<const MeshEditorPickData> Data;
	};
#endif

	class Mesh
	{
		friend class MeshLoader;
	public:
		Mesh() = default;
		Mesh(const Mesh&) = default;
		Mesh(const std::string& path);

		[[nodiscard]] bool IsVisible(const glm::mat4& transform, const glm::mat4& viewProjection) const;

		auto& GetBoneInfoMap() { return mBoneInfoMap; }
		int& GetBoneCount() { return mBoneCounter; }
#if WITH_EDITOR
public:
		using EmbreeSubMeshHitInfo = MeshHitRange;

		[[nodiscard]] Ref<const MeshEditorPickData> GetEditorPickData() const;
		static bool ResolveEmbreeHitInfoFromRanges(const std::vector<EmbreeSubMeshHitInfo>& hitInfos, uint32_t primID, uint32_t& outSubMeshIndex, uint32_t& outMaterialIndex);
		[[nodiscard]] bool GetEmbreeHitInfo(uint32_t primID, uint32_t& outSubMeshIndex, uint32_t& outMaterialIndex) const;
#endif
	public:
		// Animation
		bool bAnimated = false;
		bool bPlayAnim = false;
		bool bStopAnim = false;

		Animation mAnimation;
		Animator mAnimator;

		float mAnimPlaySpeed = 1.0f;
		bool bUseExternalBoneMatrices = false;
		std::vector<glm::mat4> mExternalBoneMatrices;
		std::string mSkeletonPath;

		std::vector<Ref<Material>> mMaterial;
		std::vector<Ref<SubMesh>> mSubMeshes;
private:
#if WITH_EDITOR
		void InvalidateEditorPickData();
#endif

		std::string mDirectory;

#if WITH_EDITOR
		Ref<MeshEditorPickDataCache> mEditorPickDataCache = CreateRef<MeshEditorPickDataCache>();
#endif

		// Animation
		int mBoneCounter = 0;
		std::map<std::string, BoneInfo> mBoneInfoMap;
	};
}
