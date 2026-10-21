#pragma once

#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    class Level;

    struct RenderViewDescription
    {
        Json WorldRenderOverrides = Json::object();
        Json CameraRenderOverrides = Json::object();
        // The actual camera selected by the caller; editor/preview views may use none.
        int CameraEntityId = -1;
        glm::mat4 View{ 1.0f };
        glm::mat4 Projection{ 1.0f };
        glm::vec3 CameraPosition{ 0.0f };
        float NearPlane = 0.01f;
        float FarPlane = 1000.0f;
        int SelectedEntity = -1;
        bool bEditorView = false;
        uint64_t FrameNumber = 0;
		// Deterministic auxiliary renders (for example material thumbnails) can
		// override only shader material time without mutating PresentationClock.
        std::optional<double> MaterialTimeSeconds;
        RenderPostProcessSettings ProjectPostProcess;
        std::optional<float> CameraExposureOverride;
        bool bShadowsEnabled = true;
        // Temporal identities are produced by RenderSystem3D rather than
        // inferred from ordinary object or camera motion in the renderer.
        uint64_t SceneGeneration = 1;
        uint64_t JitterSequenceGeneration = 1;
        uint64_t ProjectionGeneration = 1;
        bool bCameraCut = false;
    };

    class RenderWorldExtractor final
    {
    public:
        [[nodiscard]] RenderWorld Extract(Level& level, const RenderViewDescription& view);
        [[nodiscard]] Ref<const RenderObject> BuildResourceWarmupObject(const Ref<Mesh>& mesh);
        void Reset();

    private:
        struct CachedLocalTransform
        {
            int EntityId = -1;
            glm::vec3 Translation{ 0.0f };
            glm::vec3 Rotation{ 0.0f };
            glm::vec3 Scale{ 1.0f };
            glm::mat4 Local{ 1.0f };
            glm::mat4 World{ 1.0f };
            bool bHadPresentationOffset = false;
            const Mesh* BoundsMesh = nullptr;
            uint64_t BoundsRevision = 0;
            RenderBounds WorldBounds;
            std::string StaticLightmapPath;
        };

        [[nodiscard]] uint32_t GetOrCreateObjectIndex(int entityId);
        [[nodiscard]] uint32_t GetOrCreateMeshIndex(const Mesh* mesh);
        [[nodiscard]] uint32_t GetOrCreateMaterialIndex(const Ref<Material>& material, bool bBlackSilhouette);

        std::unordered_map<int, glm::mat4> mPreviousTransforms;
        std::vector<CachedLocalTransform> mCachedLocalTransforms;
        std::vector<std::string> mCachedStaticLightmapAssets;
        bool bStaticLightmapAssetsDirty = true;
        std::unordered_map<int, std::vector<glm::mat4>> mPreviousBonePalettes;
        std::unordered_map<int, const Mesh*> mPreviousBoneMeshes;
        std::unordered_map<int, uint32_t> mObjectIndices;
        std::unordered_map<const Mesh*, uint32_t> mMeshIndices;
        // A weak ownership key distinguishes lifetimes even when an allocator
        // reuses a Material address. IDs never recycle inside a scene generation.
        std::map<std::weak_ptr<Material>, std::array<uint32_t, 2>,
            std::owner_less<std::weak_ptr<Material>>> mMaterialIndices;
        glm::mat4 mPreviousViewProjection{ 1.0f };
        uint32_t mNextObjectIndex = 0;
        uint32_t mNextMeshIndex = 0;
        uint32_t mNextMaterialIndex = 0;
        uint64_t mPreviousSceneGeneration = 0;
        bool bHasPreviousView = false;
    };
}
