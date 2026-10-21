#include "Hepch.h"

#include "NavPathDebugSystem.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

namespace HEngine
{
    bool NavPathDebugSystem::Rebuild(const NavigationBuildSettings& settings, const NavigationBounds* bounds)
    {
        bBuildAttempted = true;
        if (!mNavigation.BuildNavMesh(mLevel, settings, bounds)) return false;
        mDebugTriangles = mNavigation.GetDebugTriangles();
        return true;
    }

    bool NavPathDebugSystem::FindEntityPositionByTag(const char* tag, glm::vec3& position) const
    {
        for (auto handle : mLevel->mRegistry.view<TagComponent, TransformComponent>())
            if (mLevel->mRegistry.get<TagComponent>(handle).Tag == tag)
            { position = mLevel->GetWorldTranslation(Entity{handle, mLevel}); return true; }
        return false;
    }

    bool NavPathDebugSystem::PrepareDebug()
    {
        if (!ModeManager::b3DMode) return false;
        glm::vec3 start, end;
        if (!bDebugVisible && (!FindEntityPositionByTag("NavStart", start) || !FindEntityPositionByTag("NavEnd", end))) return false;
        if (!bBuildAttempted)
        {
            bBuildAttempted = true;
            if (!mNavigation.BuildNavMesh(mLevel)) HE_CORE_WARN("Navigation debug bake failed: {0}", mNavigation.GetLastError());
            else mDebugTriangles = mNavigation.GetDebugTriangles();
        }
        return mNavigation.IsReady();
    }

    void NavPathDebugSystem::DrawDebug()
    {
        const glm::vec3 offset(0, .06f, 0);
        if (bDebugVisible)
            for (size_t i = 0; i + 2 < mDebugTriangles.size() && i < 60000; i += 3)
                for (size_t edge = 0; edge < 3; ++edge)
                    Renderer2D::DrawLine(mDebugTriangles[i + edge] + offset,
                        mDebugTriangles[i + (edge + 1) % 3] + offset, glm::vec4(.2f, .9f, .4f, .65f));
        glm::vec3 start, end;
        std::vector<glm::vec3> path;
        if (FindEntityPositionByTag("NavStart", start) && FindEntityPositionByTag("NavEnd", end) && mNavigation.FindPath(start, end, path))
            for (size_t i = 1; i < path.size(); ++i)
                Renderer2D::DrawLine(path[i - 1] + offset, path[i] + offset, glm::vec4(.18f, .78f, 1, 1));
    }

    void NavPathDebugSystem::OnUpdateRuntime(Timestep ts)
    {
        if (!PrepareDebug()) return;
        auto camera = mLevel->GetPrimaryCameraEntity();
        if (!camera) return;
        Renderer2D::BeginScene(camera.GetComponent<CameraComponent>().Camera, mLevel->GetWorldTransform(camera));
        DrawDebug(); Renderer2D::EndScene();
    }

#if WITH_EDITOR
    void NavPathDebugSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        if (!PrepareDebug()) return;
        Renderer2D::BeginScene(camera);
        DrawDebug(); Renderer2D::EndScene();
    }
#endif
}
