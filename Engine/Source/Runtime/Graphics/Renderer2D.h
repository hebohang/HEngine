#pragma once

#include "Runtime/Camera/Camera.h"
#include "Runtime/Camera/EditorCamera.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace HEngine
{
    struct RenderPrimitive2DFrame
    {
        RenderCamera Camera;
        std::vector<RenderPrimitive2D> Primitives;
        bool bHasCamera = false;
    };

    // Per-owner CPU collector. A Level or isolated preview binds one with
    // Renderer2D::CollectorScope, then moves the completed snapshot into a
    // RenderWorld before rendering starts.
    class RenderPrimitiveCollector final
    {
    public:
        void BeginFrame();
        void EndFrame();
        void CancelFrame() noexcept;
        [[nodiscard]] bool IsFrameOpen() const { return bFrameOpen; }
        [[nodiscard]] bool HasPendingPrimitives() const { return !mPrimitives.empty(); }

    private:
        void BeginScene(
            const RenderCamera& camera,
            RenderPrimitive2DCategory category,
            RenderPrimitiveDepthMode depthMode,
            int layer);
        void EndScene();
        void Add(RenderPrimitive2D primitive);
        [[nodiscard]] RenderPrimitive2DFrame TakeFrameSnapshot();

        std::vector<RenderPrimitive2D> mPrimitives;
        RenderCamera mCamera;
        RenderPrimitive2DCategory mCategory = RenderPrimitive2DCategory::Debug;
        RenderPrimitiveDepthMode mDepthMode = RenderPrimitiveDepthMode::DepthTest;
        size_t mScenePrimitiveStart = 0;
        int mLayer = 0;
        bool bFrameOpen = false;
        bool bSceneOpen = false;
        bool bHasCamera = false;

        friend class Renderer2D;
    };

    class Renderer2D final
    {
    public:
        class CollectorScope final
        {
        public:
            explicit CollectorScope(RenderPrimitiveCollector& collector);
            ~CollectorScope();

            CollectorScope(const CollectorScope&) = delete;
            CollectorScope& operator=(const CollectorScope&) = delete;

        private:
            RenderPrimitiveCollector* mPrevious = nullptr;
        };

        static void BeginScene(
            const Camera& camera,
            const glm::mat4& transform,
            RenderPrimitive2DCategory category = RenderPrimitive2DCategory::Debug,
            RenderPrimitiveDepthMode depthMode = RenderPrimitiveDepthMode::DepthTest,
            int layer = 0);
        static void BeginScene(
            const EditorCamera& camera,
            RenderPrimitive2DCategory category = RenderPrimitive2DCategory::Debug,
            RenderPrimitiveDepthMode depthMode = RenderPrimitiveDepthMode::DepthTest,
            int layer = 0);
        static void EndScene();
        [[nodiscard]] static RenderPrimitive2DFrame TakeFrameSnapshot();

        static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);
        static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color);
        static void DrawQuad(
            const glm::vec2& position,
            const glm::vec2& size,
            std::string_view textureAssetPath,
            float tilingFactor = 1.0f,
            const glm::vec4& tintColor = glm::vec4(1.0f));
        static void DrawQuad(
            const glm::vec3& position,
            const glm::vec2& size,
            std::string_view textureAssetPath,
            float tilingFactor = 1.0f,
            const glm::vec4& tintColor = glm::vec4(1.0f));
        static void DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityId = -1);
        static void DrawQuad(
            const glm::mat4& transform,
            std::string_view textureAssetPath,
            float tilingFactor = 1.0f,
            const glm::vec4& tintColor = glm::vec4(1.0f),
            int entityId = -1);

        // Rotation is in radians.
        static void DrawRotatedQuad(
            const glm::vec2& position,
            const glm::vec2& size,
            float rotation,
            const glm::vec4& color);
        static void DrawRotatedQuad(
            const glm::vec3& position,
            const glm::vec2& size,
            float rotation,
            const glm::vec4& color);
        static void DrawRotatedQuad(
            const glm::vec2& position,
            const glm::vec2& size,
            float rotation,
            std::string_view textureAssetPath,
            float tilingFactor = 1.0f,
            const glm::vec4& tintColor = glm::vec4(1.0f));
        static void DrawRotatedQuad(
            const glm::vec3& position,
            const glm::vec2& size,
            float rotation,
            std::string_view textureAssetPath,
            float tilingFactor = 1.0f,
            const glm::vec4& tintColor = glm::vec4(1.0f));

        static void DrawCircle(
            const glm::mat4& transform,
            const glm::vec4& color,
            float thickness = 1.0f,
            float fade = 0.005f,
            int entityId = -1);
        static void DrawLine(
            const glm::vec3& p0,
            const glm::vec3& p1,
            const glm::vec4& color,
            float lineWidth = 2.0f,
            int entityId = -1);
        static void DrawRect(const glm::mat4& transform, const glm::vec4& color, int entityId = -1);
        static void DrawRect(
            const glm::vec3& position,
            const glm::vec2& size,
            const glm::vec4& color,
            int entityId = -1);
        struct Statistics
        {
            uint32_t DrawCalls = 0;
            uint32_t QuadCount = 0;
            uint32_t LineCount = 0;

            [[nodiscard]] uint32_t GetTotalVertexCount() const { return QuadCount * 4 + LineCount * 2; }
            [[nodiscard]] uint32_t GetTotalIndexCount() const { return QuadCount * 6; }
        };

        static void ResetStats();
        [[nodiscard]] static Statistics GetStats();

    private:
        [[nodiscard]] static RenderPrimitiveCollector& RequireCollector();
    };
}
