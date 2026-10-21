#pragma once

#include "Runtime/Resource/ResType/NavigationSettings.h"

#include <glm/glm.hpp>

#include <vector>
#include <array>
#include <string>

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

    struct NavigationBounds
    {
        glm::vec3 Minimum;
        glm::vec3 Maximum;
        [[nodiscard]] bool IsValid() const;
    };

    struct NavigationGeometry
    {
        std::vector<float> Vertices; // World-space xyz triples.
        std::vector<int> Indices; // Triangle triples, upward front faces.
        std::vector<unsigned char> Areas; // Optional per triangle: 0..62; 63 blocks traversal.
        // Clips a world-space triangle to a build volume, preserving winding and area.
        void AddClippedTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
            unsigned char area, const NavigationBounds& bounds);
    };

    struct NavigationQuerySettings
    {
        glm::vec3 SearchExtents{2.0f, 4.0f, 2.0f};
        std::array<float, 64> AreaCosts = [] { std::array<float, 64> costs; costs.fill(1); return costs; }();
    };

    class NavigationService
    {
    public:
        NavigationService() = default;
        static NavigationService& Get();
        static NavigationBuildSettings LoadProjectSettings();
        static void ValidateSettings(const NavigationBuildSettings& settings);

        bool BuildNavMesh(Level* level);
        bool BuildNavMesh(Level* level, const NavigationBuildSettings& settings, const NavigationBounds* bounds = nullptr);
        // Failed rebuilds retain the last usable mesh. CPU-only, usable in headless tools/tests.
        bool BuildGeometry(const NavigationGeometry& geometry, const NavigationBuildSettings& settings = {});
        bool FindPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& outPath);
        bool FindPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& outPath, const NavigationQuerySettings& settings);
        [[nodiscard]] std::vector<glm::vec3> GetDebugTriangles() const;
        [[nodiscard]] const std::string& GetLastError() const { return mLastError; }
        ~NavigationService();
        void Release();
        [[nodiscard]] bool IsReady() const;

    private:
        NavigationService(const NavigationService&) = delete;
        NavigationService& operator=(const NavigationService&) = delete;
        bool BuildGeometryInternal(const NavigationGeometry& geometry, const NavigationBuildSettings& settings);

    private:
        rcHeightfield* mHeightfield = nullptr;
        rcCompactHeightfield* mCompactHeightfield = nullptr;
        rcContourSet* mContourSet = nullptr;
        rcPolyMesh* mPolyMesh = nullptr;
        rcPolyMeshDetail* mDetailMesh = nullptr;

        dtNavMesh* mNavMesh = nullptr;
        dtNavMeshQuery* mNavQuery = nullptr;
        std::string mLastError;
        NavigationQuerySettings mQuerySettings;
    };
}
