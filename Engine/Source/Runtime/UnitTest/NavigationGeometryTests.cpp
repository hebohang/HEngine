#include "Hepch.h"
#include "Runtime/AI/Navigation/NavigationService.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "_generated/serializer/NavigationSettings.gen.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace HEngine::Tests
{
    void NavigationGeometryContract()
    {
#if HE_ENABLE_NAVIGATION
        auto require = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
        auto& navigation = NavigationService::Get();
        struct Cleanup { ~Cleanup() { NavigationService::Get().Release(); } } cleanup;
        NavigationGeometry geometry;
        auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, unsigned char area = 0) {
            const int base = static_cast<int>(geometry.Vertices.size() / 3);
            for (auto v : {a, b, c, d}) geometry.Vertices.insert(geometry.Vertices.end(), {v.x, v.y, v.z});
            geometry.Indices.insert(geometry.Indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
            geometry.Areas.insert(geometry.Areas.end(), {area, area});
        };
        quad({-6,0,-5}, {6,0,-5}, {6,0,5}, {-6,0,5});
        // Closed waist-high wall, unreachable top and actual vertical geometry.
        quad({-.5f,2,-1.5f}, {.5f,2,-1.5f}, {.5f,2,1.5f}, {-.5f,2,1.5f}, 63);
        quad({-.5f,0,-1.5f}, {-.5f,2,-1.5f}, {-.5f,2,1.5f}, {-.5f,0,1.5f});
        quad({.5f,0,-1.5f}, {.5f,0,1.5f}, {.5f,2,1.5f}, {.5f,2,-1.5f});
        quad({-.5f,0,-1.5f}, {.5f,0,-1.5f}, {.5f,2,-1.5f}, {-.5f,2,-1.5f});
        quad({-.5f,0,1.5f}, {-.5f,2,1.5f}, {.5f,2,1.5f}, {.5f,0,1.5f});
        NavigationBuildSettings settings;
        settings.AgentMaxClimb = 0.3f;
        NavigationSurfaceRes surface; surface.TagPrefix = "Walkable"; surface.Area = 2;
        settings.Surfaces.push_back(surface);
        settings.AreaCosts.assign(64, 1); settings.AreaCosts[2] = 4;
        const auto roundtrip = Json(settings).get<NavigationBuildSettings>();
        require(roundtrip.Surfaces.at(0).Area == 2 && roundtrip.AreaCosts.at(2) == 4 && roundtrip.AgentMaxClimb == settings.AgentMaxClimb,
            "Navigation settings reflection roundtrip lost surface rules or costs.");
        require(navigation.BuildGeometry(geometry, settings), "Could not bake wall geometry.");
        require(navigation.IsReady() && !navigation.GetDebugTriangles().empty(), "Navigation debug geometry is empty.");
        std::vector<glm::vec3> path;
        require(navigation.FindPath({-4,0,0}, {4,0,0}, path), "Walkable route around wall was lost.");
        require(path.size() > 2, "Path went straight through actual wall geometry.");
        bool detour = false;
        for (const auto& point : path) detour |= std::abs(point.z) > 1.5f;
        require(detour, "Wall clearance did not affect the computed path.");
        NavigationService isolated;
        require(!isolated.IsReady(), "A new scene must not inherit another scene's navigation mesh.");

        const NavigationBounds bounds{{-5,-1,-4}, {5,3,4}};
        NavigationGeometry clipped;
        for (size_t i = 0; i < geometry.Indices.size(); i += 3)
        {
            glm::vec3 points[3];
            for (int j = 0; j < 3; ++j)
            {
                const auto index = geometry.Indices[i + j] * 3;
                points[j] = {geometry.Vertices[index], geometry.Vertices[index + 1], geometry.Vertices[index + 2]};
            }
            clipped.AddClippedTriangle(points[0], points[1], points[2], geometry.Areas[i / 3], bounds);
        }
        const auto vertexCount = clipped.Vertices.size();
        clipped.AddClippedTriangle({100,0,0}, {100,0,1}, {101,0,0}, 0, bounds);
        require(clipped.Vertices.size() == vertexCount, "Outside triangles leaked into a bounded build.");
        for (size_t i = 0; i < clipped.Vertices.size(); ++i)
            require(clipped.Vertices[i] >= bounds.Minimum[i % 3] && clipped.Vertices[i] <= bounds.Maximum[i % 3],
                "Clipped vertices escaped the build volume.");
        require(navigation.BuildGeometry(clipped, settings), "Clipped floor failed to preserve walkable winding.");
        require(navigation.FindPath({-4,0,0}, {4,0,0}, path) && path.size() > 2,
            "Bounded mesh lost obstacle routing or area flags.");
        require(!NavigationBounds{{0,0,0}, {1,0,1}}.IsValid(), "Zero-volume bounds accepted.");
        require(!NavigationBounds{{0,0,0}, {1,std::numeric_limits<float>::infinity(),1}}.IsValid(), "Infinite bounds accepted.");

        {
            Level indexedLevel;
            auto entity = indexedLevel.CreateEntity("NavGeometrySeamedFloor");
            auto& mesh = entity.AddComponent<MeshComponent>();
            std::vector<StaticVertex> vertices;
            std::vector<uint32_t> indices;
            // 360,000 faces used to expand past the unchanged one-million vertex
            // cap. UV/hard-normal splits and +/-0 share exactly the same geometry.
            constexpr int width = 450, depth = 400;
            vertices.reserve(width * depth * 6 + 3); indices.reserve(width * depth * 6 + 3);
            auto append = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
                for (const auto& point : {a, b, c})
                {
                    StaticVertex vertex{}; vertex.Pos = point;
                    vertex.Pos.y = vertices.size() % 2 ? -0.0f : 0.0f;
                    vertex.Normal = vertices.size() % 2 ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                    vertex.TexCoord = glm::vec2(static_cast<float>(vertices.size() % 7));
                    indices.push_back(static_cast<uint32_t>(vertices.size())); vertices.push_back(vertex);
                }
            };
            for (int x = 0; x < width; ++x)
                for (int z = 0; z < depth; ++z)
                {
                    const glm::vec3 a(x * .2f, 0, z * .2f), b((x + 1) * .2f, 0, z * .2f);
                    const glm::vec3 c((x + 1) * .2f, 0, (z + 1) * .2f), d(x * .2f, 0, (z + 1) * .2f);
                    append(a, c, b); append(a, d, c);
                }
            append({-1,0,20}, {1,0,21}, {1,0,20}); // Same section also exercises partial clipping.
            mesh.mMesh->mSubMeshes.push_back(CreateRef<SubMesh>(std::move(vertices), std::move(indices)));
            NavigationService indexed;
            NavigationBuildSettings indexedSettings;
            const NavigationBounds indexedBounds{{0,-1,0}, {90,3,80}};
            require(indexed.BuildNavMesh(&indexedLevel, indexedSettings, &indexedBounds),
                "Bounded UV/hard-normal seams exhausted the navigation vertex budget.");
            require(indexed.FindPath({1,0,1}, {88,0,78}, path), "Indexed position reuse lost walkable floor connectivity.");
            for (const auto& point : indexed.GetDebugTriangles())
                require(point.x >= 0 && point.x <= 90 && point.z >= 0 && point.z <= 80,
                    "Mixed interior/clipped input escaped navigation bounds.");

            std::vector<StaticVertex> testVertices(4);
            testVertices[0].Pos = {-6,0,-5}; testVertices[1].Pos = {6,0,-5}; testVertices[2].Pos = {6,0,5}; testVertices[3].Pos = {-6,0,5};
            mesh.mMesh->mSubMeshes[0] = CreateRef<SubMesh>(testVertices, std::vector<uint32_t>{0,2,1,0,3,2});
            entity.GetComponent<TransformComponent>().Scale = {-1,1,1};
            require(indexed.BuildNavMesh(&indexedLevel, indexedSettings, &bounds) && indexed.FindPath({-4,0,0}, {4,0,0}, path),
                "Indexed bounded extraction lost mirrored winding.");
            mesh.mMesh->mSubMeshes[0] = CreateRef<SubMesh>(testVertices, std::vector<uint32_t>{0,2,999});
            require(!indexed.BuildNavMesh(&indexedLevel, indexedSettings, &bounds) && indexed.FindPath({-4,0,0}, {4,0,0}, path),
                "Invalid source index passed bounded extraction or discarded the usable mesh.");
            testVertices[0].Pos.x = std::numeric_limits<float>::quiet_NaN();
            mesh.mMesh->mSubMeshes[0] = CreateRef<SubMesh>(testVertices, std::vector<uint32_t>{0,2,1});
            require(!indexed.BuildNavMesh(&indexedLevel, indexedSettings, &bounds), "Position welding accepted NaN geometry.");
        }

        auto invalid = geometry;
        invalid.Indices[0] = 1000000;
        require(!navigation.BuildGeometry(invalid) && !navigation.GetLastError().empty(), "Invalid triangle index was accepted.");
        require(navigation.FindPath({-4,0,0}, {4,0,0}, path), "Failed rebuild discarded the last usable mesh.");
        auto excessive = settings; excessive.CellSize = .01f; excessive.MaxGridCells = 100;
        require(!navigation.BuildGeometry(geometry, excessive), "Grid memory budget was ignored.");
        auto nan = settings; nan.AgentHeight = std::numeric_limits<float>::quiet_NaN();
        require(!navigation.BuildGeometry(geometry, nan), "Nonfinite agent settings were accepted.");
        NavigationQuerySettings query; query.AreaCosts[0] = -1;
        require(!navigation.FindPath({-4,0,0}, {4,0,0}, path, query) && path.empty(), "Invalid area cost returned a path.");

        geometry = {};
        quad({-8,0,-3}, {-2,0,-3}, {-2,0,3}, {-8,0,3});
        quad({2,0,-3}, {8,0,-3}, {8,0,3}, {2,0,3});
        require(navigation.BuildGeometry(geometry, settings), "Could not bake disconnected islands.");
        require(!navigation.FindPath({-5,0,0}, {5,0,0}, path) && path.empty(),
            "Partial Detour path was incorrectly reported as reaching an unreachable target.");
#else
        throw std::runtime_error("NavigationGeometry requires HE_ENABLE_NAVIGATION.");
#endif
    }
}
