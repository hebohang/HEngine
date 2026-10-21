#pragma once

#include "Runtime/Graphics/Mesh/MeshVertex.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>
#include <map>
#include <string>

#define MAX_BONE_INFLUENCE 16

namespace HEngine
{
    class Mesh;

    struct SubMeshLod
    {
        std::vector<uint32_t> Indices;
        float ObjectSpaceError = 0.0f;
    };

    struct MorphTarget
    {
        std::string Name;
        float DefaultWeight = 0.0f;
        std::vector<glm::vec3> Positions;
        std::vector<glm::vec3> Normals;
        std::vector<glm::vec3> Tangents;
    };

    struct SkinnedVertex
    {
        glm::vec3 Pos;
        glm::vec3 PreviousPosition{0.0f};
        glm::vec3 Normal;
        glm::vec2 TexCoord{0.0f};
        glm::vec2 TexCoord1{0.0f};
        glm::vec4 Color{1.0f};
        glm::vec3 Tangent;
        glm::vec3 Bitangent;

        int mBoneIDs[MAX_BONE_INFLUENCE] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
        float mWeights[MAX_BONE_INFLUENCE] = {};
        // CPU cloth paint is separate from authored vertex color/material IDs.
        // Negative values retain the legacy COLOR_0.r fallback for old assets.
        float ClothWeight = -1.0f;
    };

    // CPU mesh section. GPU buffers are owned and cached by SceneRenderer;
    // this asset object remains valid before a RenderDevice exists.
    class SubMesh
    {
        friend class Mesh;

    public:
        SubMesh(std::vector<StaticVertex> vertices, std::vector<uint32_t> indices, uint32_t materialIndex = 0);
        SubMesh(std::vector<SkinnedVertex> vertices, std::vector<uint32_t> indices, uint32_t materialIndex = 0);

        [[nodiscard]] bool IsVisible(const glm::mat4& transform, const glm::mat4& viewProjection) const;
        [[nodiscard]] bool HasBounds() const { return bHasBounds; }
        [[nodiscard]] const glm::vec3& GetBoundsMin() const { return mBoundsMin; }
        [[nodiscard]] const glm::vec3& GetBoundsMax() const { return mBoundsMax; }
        [[nodiscard]] const std::vector<uint32_t>& GetIndices() const { return mIndices; }
        [[nodiscard]] bool IsSkinned() const { return !mSkinnedVertices.empty(); }
        [[nodiscard]] const std::vector<SubMeshLod>& GetLods() const noexcept { return mLods; }
        void SetLods(std::vector<SubMeshLod> lods);

        void SetMorphTargets(std::vector<MorphTarget> targets);
        [[nodiscard]] const std::vector<MorphTarget>& GetMorphTargets() const { return mMorphTargets; }
        [[nodiscard]] bool HasEffectiveMorphTargets() const { return mHasEffectiveMorphTargets; }
        [[nodiscard]] const std::vector<SkinnedVertex>& GetBaseSkinnedVertices() const { return mBaseSkinnedVertices.empty() ? mSkinnedVertices : mBaseSkinnedVertices; }
        [[nodiscard]] uint64_t GetDeformationRevision() const { return mDeformationRevision; }
        [[nodiscard]] uint64_t GetDeformationHistoryGeneration() const { return mDeformationHistoryGeneration; }
        void BeginDeformation();
        void ApplyMorphWeights(const std::map<std::string, float>& weights);
        void EndDeformation(bool resetHistory = false);
        // Pure morph path only: after a changed frame, publish prev=current once,
        // then retain immutable vertex data until effective weights change again.
        void UpdatePureMorphWeights(const std::map<std::string, float>& weights, bool resetHistory = false);

        uint32_t mMaterialIndex;
        std::vector<StaticVertex> mStaticVertices;
        std::vector<SkinnedVertex> mSkinnedVertices;

    private:
        std::vector<MorphTarget> mMorphTargets;
        std::vector<bool> mMorphTargetAffectsGeometry;
        bool mHasEffectiveMorphTargets = false;
        std::vector<SkinnedVertex> mBaseSkinnedVertices;
        std::vector<StaticVertex> mBaseStaticVertices;
        uint64_t mDeformationRevision = 0;
        uint64_t mDeformationHistoryGeneration = 0;
        std::vector<float> mCachedMorphWeights;
        const SubMesh* mMorphCacheOwner = nullptr;
        bool bMorphHistoryPending = false;
        std::vector<uint32_t> mIndices;
        std::vector<SubMeshLod> mLods;
        glm::vec3 mBoundsMin{ 0.0f };
        glm::vec3 mBoundsMax{ 0.0f };
        bool bHasBounds = false;
    };
}
