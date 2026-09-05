#pragma once

#include "Runtime/EcsFramework/System/System.h"

#include <glm/glm.hpp>

struct rcHeightfield;
struct rcCompactHeightfield;
struct rcContourSet;
struct rcPolyMesh;
struct rcPolyMeshDetail;

class dtNavMesh;
class dtNavMeshQuery;

namespace HEngine
{
    class NavPathDebugSystem : public System
    {
    public:
        NavPathDebugSystem(Level* level)
            : System(level)
        {
        }

        virtual ~NavPathDebugSystem();

        void OnUpdateRuntime(Timestep ts) override;

#if WITH_EDITOR
        void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
#endif

    private:
        bool RebuildNavMeshFromScene();
        void ReleaseBuildData();
        void ReleaseDetour();
        bool FindEntityPositionByTag(const char* tagName, glm::vec3& outPosition) const;
        bool BuildAndDrawPath(const glm::vec3& startWorld, const glm::vec3& endWorld, EditorCamera& camera);

    private:
        rcHeightfield* mHeightfield = nullptr;
        rcCompactHeightfield* mCompactHeightfield = nullptr;
        rcContourSet* mContourSet = nullptr;
        rcPolyMesh* mPolyMesh = nullptr;
        rcPolyMeshDetail* mDetailMesh = nullptr;

        dtNavMesh* mNavMesh = nullptr;
        dtNavMeshQuery* mNavQuery = nullptr;
        bool mLastBuildSucceeded = false;
        bool mBuildErrorReported = false;
    };
}
