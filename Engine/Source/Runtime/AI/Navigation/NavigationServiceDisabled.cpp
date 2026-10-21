#include "Hepch.h"
#include "NavigationService.h"
#include "Runtime/Core/BuildFeatures.h"

namespace HEngine
{
    NavigationService& NavigationService::Get() { static NavigationService service; return service; }
    NavigationService::~NavigationService() = default;
    bool NavigationService::BuildNavMesh(Level*)
    {
        RequireBuildFeature(BuildFeature::Navigation, "Build navigation mesh");
        return false;
    }
    bool NavigationService::FindPath(const glm::vec3&, const glm::vec3&, std::vector<glm::vec3>& outPath)
    {
        outPath.clear();
        RequireBuildFeature(BuildFeature::Navigation, "Find navigation path");
        return false;
    }
    bool NavigationService::BuildNavMesh(Level* level, const NavigationBuildSettings&) { return BuildNavMesh(level); }
    bool NavigationService::BuildGeometry(const NavigationGeometry&, const NavigationBuildSettings&)
    {
        RequireBuildFeature(BuildFeature::Navigation, "Build navigation geometry");
        return false;
    }
    bool NavigationService::FindPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& outPath, const NavigationQuerySettings&)
    { return FindPath(start, end, outPath); }
    std::vector<glm::vec3> NavigationService::GetDebugTriangles() const { return {}; }
    void NavigationService::Release() {}
    bool NavigationService::IsReady() const { return false; }
}
