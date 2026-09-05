#include "Hepch.h"

#include "Runtime/Graphics/Renderer2D.h"

#include <glm/gtc/matrix_transform.hpp>

#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        thread_local RenderPrimitiveCollector* sActiveCollector = nullptr;
        thread_local Renderer2D::Statistics sStatistics;

        RenderCamera MakeRenderCamera(const Camera& camera, const glm::mat4& transform)
        {
            RenderCamera result;
            result.View = glm::inverse(transform);
            result.Projection = camera.GetProjection();
            result.ViewProjection = result.Projection * result.View;
            result.PreviousViewProjection = result.ViewProjection;
            result.NonJitteredViewProjection = result.ViewProjection;
            result.PreviousNonJitteredViewProjection = result.ViewProjection;
            result.Position = glm::vec3(transform[3]);
            return result;
        }

        RenderCamera MakeRenderCamera(const EditorCamera& camera)
        {
            RenderCamera result;
            result.View = camera.GetViewMatrix();
            result.Projection = camera.GetProjection();
            result.ViewProjection = result.Projection * result.View;
            result.PreviousViewProjection = result.ViewProjection;
            result.NonJitteredViewProjection = result.ViewProjection;
            result.PreviousNonJitteredViewProjection = result.ViewProjection;
            result.Position = camera.GetPosition();
            result.NearPlane = camera.GetNearPlane();
            result.FarPlane = camera.GetFarPlane();
            return result;
        }
    }

    void RenderPrimitiveCollector::BeginFrame()
    {
        if (bFrameOpen)
            throw std::logic_error("RenderPrimitiveCollector frame is already open.");

        mPrimitives.clear();
        bFrameOpen = true;
        bSceneOpen = false;
        bHasCamera = false;
    }

    void RenderPrimitiveCollector::EndFrame()
    {
        if (!bFrameOpen)
            throw std::logic_error("RenderPrimitiveCollector frame is not open.");
        if (bSceneOpen)
            throw std::logic_error("Renderer2D::EndScene must be called before ending the collector frame.");
        if (!mPrimitives.empty())
            throw std::logic_error("Renderer2D primitives were not moved into a RenderWorld before EndFrame.");

        bFrameOpen = false;
        mPrimitives.clear();
        bHasCamera = false;
    }

    void RenderPrimitiveCollector::CancelFrame() noexcept
    {
        mPrimitives.clear();
        bFrameOpen = false;
        bSceneOpen = false;
        bHasCamera = false;
    }

    void RenderPrimitiveCollector::BeginScene(
        const RenderCamera& camera,
        RenderPrimitive2DCategory category,
        RenderPrimitiveDepthMode depthMode,
        int layer)
    {
        if (!bFrameOpen)
            throw std::logic_error("Renderer2D requires an open RenderPrimitiveCollector frame.");
        if (bSceneOpen)
            throw std::logic_error("Renderer2D scene is already open.");

        mCamera = camera;
        mCategory = category;
        mDepthMode = depthMode;
        mLayer = layer;
        mScenePrimitiveStart = mPrimitives.size();
        bHasCamera = true;
        bSceneOpen = true;
    }

    void RenderPrimitiveCollector::EndScene()
    {
        if (!bSceneOpen)
            throw std::logic_error("Renderer2D scene is not open.");

        if (mPrimitives.size() != mScenePrimitiveStart)
            ++sStatistics.DrawCalls;
        bSceneOpen = false;
    }

    void RenderPrimitiveCollector::Add(RenderPrimitive2D primitive)
    {
        if (!bSceneOpen)
            throw std::logic_error("Renderer2D draw submission requires BeginScene.");

        primitive.Category = mCategory;
        primitive.DepthMode = mDepthMode;
        primitive.Layer = mLayer;
        mPrimitives.push_back(std::move(primitive));
    }

    RenderPrimitive2DFrame RenderPrimitiveCollector::TakeFrameSnapshot()
    {
        if (!bFrameOpen)
            throw std::logic_error("Cannot acquire Renderer2D data outside a collector frame.");
        if (bSceneOpen)
            throw std::logic_error("Renderer2D::EndScene must be called before acquiring frame data.");

        RenderPrimitive2DFrame frame;
        frame.Camera = mCamera;
        frame.Primitives = std::move(mPrimitives);
        frame.bHasCamera = bHasCamera;
        mPrimitives.clear();
        bHasCamera = false;
        return frame;
    }

    Renderer2D::CollectorScope::CollectorScope(RenderPrimitiveCollector& collector)
        : mPrevious(sActiveCollector)
    {
        if (!collector.IsFrameOpen())
            throw std::logic_error("Renderer2D collector scope requires BeginFrame.");
        sActiveCollector = &collector;
    }

    Renderer2D::CollectorScope::~CollectorScope()
    {
        sActiveCollector = mPrevious;
    }

    RenderPrimitiveCollector& Renderer2D::RequireCollector()
    {
        if (!sActiveCollector)
            throw std::logic_error("Renderer2D submission requires a scoped RenderPrimitiveCollector owner.");
        return *sActiveCollector;
    }

    void Renderer2D::BeginScene(
        const Camera& camera,
        const glm::mat4& transform,
        RenderPrimitive2DCategory category,
        RenderPrimitiveDepthMode depthMode,
        int layer)
    {
        RequireCollector().BeginScene(MakeRenderCamera(camera, transform), category, depthMode, layer);
    }

    void Renderer2D::BeginScene(
        const EditorCamera& camera,
        RenderPrimitive2DCategory category,
        RenderPrimitiveDepthMode depthMode,
        int layer)
    {
        RequireCollector().BeginScene(MakeRenderCamera(camera), category, depthMode, layer);
    }

    void Renderer2D::EndScene()
    {
        RequireCollector().EndScene();
    }

    RenderPrimitive2DFrame Renderer2D::TakeFrameSnapshot()
    {
        return RequireCollector().TakeFrameSnapshot();
    }

    void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
    {
        DrawQuad(glm::vec3(position, 0.0f), size, color);
    }

    void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color)
    {
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
            * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
        DrawQuad(transform, color);
    }

    void Renderer2D::DrawQuad(
        const glm::vec2& position,
        const glm::vec2& size,
        std::string_view textureAssetPath,
        float tilingFactor,
        const glm::vec4& tintColor)
    {
        DrawQuad(glm::vec3(position, 0.0f), size, textureAssetPath, tilingFactor, tintColor);
    }

    void Renderer2D::DrawQuad(
        const glm::vec3& position,
        const glm::vec2& size,
        std::string_view textureAssetPath,
        float tilingFactor,
        const glm::vec4& tintColor)
    {
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
            * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
        DrawQuad(transform, textureAssetPath, tilingFactor, tintColor);
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityId)
    {
        RenderPrimitive2D primitive;
        primitive.Kind = RenderPrimitive2DKind::Quad;
        primitive.Transform = transform;
        primitive.Color = color;
        primitive.EntityId = entityId;
        RequireCollector().Add(std::move(primitive));
        ++sStatistics.QuadCount;
    }

    void Renderer2D::DrawQuad(
        const glm::mat4& transform,
        std::string_view textureAssetPath,
        float tilingFactor,
        const glm::vec4& tintColor,
        int entityId)
    {
        RenderPrimitive2D primitive;
        primitive.Kind = RenderPrimitive2DKind::Quad;
        primitive.Transform = transform;
        primitive.Color = tintColor;
        primitive.TextureAssetPath = std::string(textureAssetPath);
        primitive.TilingFactor = tilingFactor;
        primitive.EntityId = entityId;
        RequireCollector().Add(std::move(primitive));
        ++sStatistics.QuadCount;
    }

    void Renderer2D::DrawRotatedQuad(
        const glm::vec2& position,
        const glm::vec2& size,
        float rotation,
        const glm::vec4& color)
    {
        DrawRotatedQuad(glm::vec3(position, 0.0f), size, rotation, color);
    }

    void Renderer2D::DrawRotatedQuad(
        const glm::vec3& position,
        const glm::vec2& size,
        float rotation,
        const glm::vec4& color)
    {
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
            * glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f))
            * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
        DrawQuad(transform, color);
    }

    void Renderer2D::DrawRotatedQuad(
        const glm::vec2& position,
        const glm::vec2& size,
        float rotation,
        std::string_view textureAssetPath,
        float tilingFactor,
        const glm::vec4& tintColor)
    {
        DrawRotatedQuad(glm::vec3(position, 0.0f), size, rotation, textureAssetPath, tilingFactor, tintColor);
    }

    void Renderer2D::DrawRotatedQuad(
        const glm::vec3& position,
        const glm::vec2& size,
        float rotation,
        std::string_view textureAssetPath,
        float tilingFactor,
        const glm::vec4& tintColor)
    {
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
            * glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f))
            * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
        DrawQuad(transform, textureAssetPath, tilingFactor, tintColor);
    }

    void Renderer2D::DrawCircle(
        const glm::mat4& transform,
        const glm::vec4& color,
        float thickness,
        float fade,
        int entityId)
    {
        RenderPrimitive2D primitive;
        primitive.Kind = RenderPrimitive2DKind::Circle;
        primitive.Transform = transform;
        primitive.Color = color;
        primitive.Thickness = thickness;
        primitive.Fade = fade;
        primitive.EntityId = entityId;
        RequireCollector().Add(std::move(primitive));
        ++sStatistics.QuadCount;
    }

    void Renderer2D::DrawLine(
        const glm::vec3& p0,
        const glm::vec3& p1,
        const glm::vec4& color,
        float lineWidth,
        int entityId)
    {
        RenderPrimitive2D primitive;
        primitive.Kind = RenderPrimitive2DKind::Line;
        primitive.LineStart = p0;
        primitive.LineEnd = p1;
        primitive.Color = color;
        primitive.LineWidth = lineWidth;
        primitive.EntityId = entityId;
        RequireCollector().Add(std::move(primitive));
        ++sStatistics.LineCount;
    }

    void Renderer2D::DrawRect(const glm::mat4& transform, const glm::vec4& color, int entityId)
    {
        glm::vec3 lineVertices[4];
        lineVertices[0] = transform * glm::vec4(-0.5f, -0.5f, 0.0f, 1.0f);
        lineVertices[1] = transform * glm::vec4( 0.5f, -0.5f, 0.0f, 1.0f);
        lineVertices[2] = transform * glm::vec4( 0.5f,  0.5f, 0.0f, 1.0f);
        lineVertices[3] = transform * glm::vec4(-0.5f,  0.5f, 0.0f, 1.0f);
        for (size_t index = 0; index < 4; ++index)
            DrawLine(lineVertices[index], lineVertices[(index + 1) % 4], color, 2.0f, entityId);
    }

    void Renderer2D::DrawRect(
        const glm::vec3& position,
        const glm::vec2& size,
        const glm::vec4& color,
        int entityId)
    {
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
            * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
        DrawRect(transform, color, entityId);
    }

    void Renderer2D::ResetStats()
    {
        sStatistics = {};
    }

    Renderer2D::Statistics Renderer2D::GetStats()
    {
        return sStatistics;
    }
}
