#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/AI/Navigation/NavigationService.h"

namespace HEngine
{
    class NavPathDebugSystem : public System
    {
    public:
        explicit NavPathDebugSystem(Level* level) : System(level) {}
        void OnUpdateRuntime(Timestep ts) override;
        void OnRuntimeStop() override { mNavigation.Release(); bBuildAttempted = false; mDebugTriangles.clear(); }
#if WITH_EDITOR
        void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
#endif
        bool Rebuild(const NavigationBuildSettings& settings, const NavigationBounds* bounds = nullptr);
        NavigationService& GetNavigation() { return mNavigation; }
        void SetDebugVisible(bool visible) { bDebugVisible = visible; }
        bool IsDebugVisible() const { return bDebugVisible; }

    private:
        bool FindEntityPositionByTag(const char* tag, glm::vec3& position) const;
        bool PrepareDebug();
        void DrawDebug();
        NavigationService mNavigation;
        std::vector<glm::vec3> mDebugTriangles;
        bool bDebugVisible = false;
        bool bBuildAttempted = false;
    };
}
