#pragma once

#include <glm/glm.hpp>

#include <vector>

struct rcHeightfield;
struct rcCompactHeightfield;
struct rcContourSet;
struct rcPolyMesh;
struct rcPolyMeshDetail;

class dtNavMesh;
class dtNavMeshQuery;

namespace HEngine
{
    class Level;

    class NavigationService
    {
    public:
        static NavigationService& Get();

        bool BuildNavMesh(Level* level);
        bool FindPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& outPath);
        void Release();
        [[nodiscard]] bool IsReady() const;

    private:
        NavigationService() = default;

    private:
        rcHeightfield* mHeightfield = nullptr;
        rcCompactHeightfield* mCompactHeightfield = nullptr;
        rcContourSet* mContourSet = nullptr;
        rcPolyMesh* mPolyMesh = nullptr;
        rcPolyMeshDetail* mDetailMesh = nullptr;

        dtNavMesh* mNavMesh = nullptr;
        dtNavMeshQuery* mNavQuery = nullptr;
    };
}
