#pragma once

#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace HEngine
{
    class Level;

    struct RenderViewDescription
    {
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
        void Reset();

    private:
        [[nodiscard]] uint32_t GetOrCreateObjectIndex(int entityId);
        [[nodiscard]] uint32_t GetOrCreateMeshIndex(const Mesh* mesh);
        [[nodiscard]] uint32_t GetOrCreateMaterialIndex(const Material* material);

        std::unordered_map<int, glm::mat4> mPreviousTransforms;
        std::unordered_map<int, uint32_t> mObjectIndices;
        std::unordered_map<const Mesh*, uint32_t> mMeshIndices;
        std::unordered_map<const Material*, uint32_t> mMaterialIndices;
        glm::mat4 mPreviousViewProjection{ 1.0f };
        uint32_t mNextObjectIndex = 0;
        uint32_t mNextMeshIndex = 0;
        uint32_t mNextMaterialIndex = 0;
        bool bHasPreviousView = false;
    };
}
