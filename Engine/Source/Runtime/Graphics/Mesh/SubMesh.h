#pragma once

#include "Runtime/Graphics/Mesh/MeshVertex.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

#define MAX_BONE_INFLUENCE 4

namespace HEngine
{
    class Mesh;

    struct SkinnedVertex
    {
        glm::vec3 Pos;
        glm::vec3 Normal;
        glm::vec2 TexCoord;
        glm::vec3 Tangent;
        glm::vec3 Bitangent;

        int mBoneIDs[MAX_BONE_INFLUENCE];
        float mWeights[MAX_BONE_INFLUENCE];
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

        uint32_t mMaterialIndex;
        std::vector<StaticVertex> mStaticVertices;
        std::vector<SkinnedVertex> mSkinnedVertices;

    private:
        std::vector<uint32_t> mIndices;
        glm::vec3 mBoundsMin{ 0.0f };
        glm::vec3 mBoundsMax{ 0.0f };
        bool bHasBounds = false;
    };
}
