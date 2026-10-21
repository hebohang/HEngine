#include "Hepch.h"

#include "Runtime/AI/Navigation/NavigationService.h"

#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr unsigned short kWalkFlag = 0x01;

        struct PositionHash
        {
            size_t operator()(const std::array<float, 3>& point) const
            {
                // float equality and std::hash agree for +0/-0. Nonfinite input is rejected before lookup.
                size_t value = 0;
                for (float coordinate : point)
                    value ^= std::hash<float>{}(coordinate) + 0x9e3779b9u + (value << 6) + (value >> 2);
                return value;
            }
        };

        struct BuildContext final : rcContext
        {
            BuildContext()
                : rcContext(false)
            {
            }
        };

        void AddQuadOnXZ(const glm::vec3& center, const glm::vec3& halfSize, std::vector<float>& outVerts, std::vector<int>& outTris)
        {
            const int base = static_cast<int>(outVerts.size() / 3);

            const std::array<glm::vec3, 4> quad = {
                glm::vec3(center.x - halfSize.x, center.y, center.z - halfSize.z),
                glm::vec3(center.x + halfSize.x, center.y, center.z - halfSize.z),
                glm::vec3(center.x + halfSize.x, center.y, center.z + halfSize.z),
                glm::vec3(center.x - halfSize.x, center.y, center.z + halfSize.z)
            };

            for (const glm::vec3& v : quad)
            {
                outVerts.push_back(v.x);
                outVerts.push_back(v.y);
                outVerts.push_back(v.z);
            }

            outTris.push_back(base + 0);
            outTris.push_back(base + 2);
            outTris.push_back(base + 1);

            outTris.push_back(base + 0);
            outTris.push_back(base + 3);
            outTris.push_back(base + 2);
        }
    }

    bool NavigationBounds::IsValid() const
    {
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(Minimum[axis]) || !std::isfinite(Maximum[axis]) ||
                Minimum[axis] >= Maximum[axis] || std::abs(Minimum[axis]) > 1000000 ||
                std::abs(Maximum[axis]) > 1000000) return false;
        return true;
    }

    void NavigationGeometry::AddClippedTriangle(const glm::vec3& a, const glm::vec3& b,
        const glm::vec3& c, unsigned char area, const NavigationBounds& bounds)
    {
        if (!bounds.IsValid()) throw std::invalid_argument("Invalid navigation build bounds.");
        for (const auto& point : {a, b, c})
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(point[axis])) throw std::invalid_argument("Nonfinite navigation vertex.");
        // A triangle clipped by a box has at most nine vertices. Preserve orientation.
        std::vector<glm::vec3> polygon{a, b, c}, clipped;
        for (int axis = 0; axis < 3 && !polygon.empty(); ++axis)
            for (int side = 0; side < 2 && !polygon.empty(); ++side)
            {
                clipped.clear();
                const float plane = side ? bounds.Maximum[axis] : bounds.Minimum[axis];
                auto distance = [&](const glm::vec3& point) { return side ? plane - point[axis] : point[axis] - plane; };
                auto previous = polygon.back();
                float previousDistance = distance(previous);
                for (const auto& current : polygon)
                {
                    const float currentDistance = distance(current);
                    if ((currentDistance >= 0) != (previousDistance >= 0))
                    {
                        auto intersection = previous + (current - previous) *
                            (previousDistance / (previousDistance - currentDistance));
                        intersection[axis] = plane;
                        clipped.push_back(intersection);
                    }
                    if (currentDistance >= 0) clipped.push_back(current);
                    previous = current; previousDistance = currentDistance;
                }
                polygon.swap(clipped);
            }
        if (polygon.size() < 3) return;
        if (Vertices.size() / 3 + polygon.size() > 1000000 || Indices.size() / 3 + polygon.size() - 2 > 1000000)
            throw std::invalid_argument("Navigation geometry exceeds one million vertices/triangles.");
        const int base = static_cast<int>(Vertices.size() / 3);
        for (auto point : polygon) Vertices.insert(Vertices.end(), {point.x, point.y, point.z});
        for (int i = 1; i + 1 < static_cast<int>(polygon.size()); ++i)
        {
            const auto normal = glm::cross(polygon[i] - polygon[0], polygon[i + 1] - polygon[0]);
            if (glm::dot(normal, normal) < 1e-12f) continue;
            Indices.insert(Indices.end(), {base, base + i, base + i + 1});
            Areas.push_back(area);
        }
    }

    NavigationService& NavigationService::Get()
    {
        static NavigationService sInstance;
        return sInstance;
    }

    bool NavigationService::BuildNavMesh(Level* level)
    {
        try { return BuildNavMesh(level, LoadProjectSettings()); }
        catch (const std::exception& error) { mLastError = error.what(); return false; }
    }

    NavigationService::~NavigationService() { Release(); }

    bool NavigationService::BuildNavMesh(Level* level, const NavigationBuildSettings& settings, const NavigationBounds* bounds)
    {
        if (!level) { mLastError = "Navigation requires a scene."; return false; }
        try { ValidateSettings(settings); }
        catch (const std::exception& error) { mLastError = error.what(); return false; }
        if (bounds && !bounds->IsValid()) { mLastError = "Invalid navigation build bounds."; return false; }

        NavigationGeometry geometry;
        auto& verts = geometry.Vertices;
        auto& tris = geometry.Indices;

		auto view = level->mRegistry.view<TagComponent, TransformComponent>();
		for (auto handle : view)
		{
			Entity entity = { handle, level };
			const auto& tag = view.get<TagComponent>(handle);
			int area = 0;
            bool useMesh = tag.Tag.starts_with("NavGeometry");
            bool included = useMesh || tag.Tag.starts_with("NavGround");
            if (!settings.Surfaces.empty())
            {
                included = false;
                for (const auto& surface : settings.Surfaces)
                    if (tag.Tag.starts_with(surface.TagPrefix))
                    { included = true; area = surface.Area; useMesh = surface.UseMesh; break; }
            }
            if (!included) continue;
			// Explicit geometry authoring leaves legacy NavGround proxy scenes compatible.
			if (useMesh)
			{
				if (!entity.HasComponent<MeshComponent>() || !entity.GetComponent<MeshComponent>().mMesh)
				{ mLastError = "NavGeometry requires a loaded static MeshComponent: " + tag.Tag; return false; }
				const auto transform = level->GetWorldTransform(entity);
				const bool mirrored = glm::determinant(glm::mat3(transform)) < 0;
				for (const auto& subMesh : entity.GetComponent<MeshComponent>().mMesh->mSubMeshes)
				{
					if (!subMesh || subMesh->IsSkinned())
					{ mLastError = "NavGeometry cannot bake skinned or missing submeshes: " + tag.Tag; return false; }
					if (bounds)
					{
                        if (subMesh->HasBounds())
                        {
                            glm::vec3 minimum(std::numeric_limits<float>::max()), maximum(std::numeric_limits<float>::lowest());
                            for (int corner = 0; corner < 8; ++corner)
                            {
                                glm::vec3 local;
                                for (int axis = 0; axis < 3; ++axis)
                                    local[axis] = (corner & (1 << axis)) ? subMesh->GetBoundsMax()[axis] : subMesh->GetBoundsMin()[axis];
                                const auto world = glm::vec3(transform * glm::vec4(local, 1));
                                minimum = glm::min(minimum, world); maximum = glm::max(maximum, world);
                            }
                            if (glm::any(glm::lessThan(maximum, bounds->Minimum)) ||
                                glm::any(glm::greaterThan(minimum, bounds->Maximum))) continue;
                        }
                        const auto& indices = subMesh->GetIndices();
                        if (indices.size() % 3 != 0) { mLastError = "Navigation requires triangle indices."; return false; }
                        // Navigation consumes positions only. Hard normals and UV seams can duplicate
                        // the same source position many times; retain an exact index for interior faces
                        // instead of expanding every visual triangle into three new navigation vertices.
                        std::unordered_map<std::array<float, 3>, int, PositionHash> positions;
                        positions.reserve(std::min(size_t(250000), subMesh->mStaticVertices.size()));
                        try
                        {
                            for (size_t i = 0; i < indices.size(); i += 3)
                            {
                                glm::vec3 points[3];
                                for (size_t j = 0; j < 3; ++j)
                                {
                                    if (indices[i + j] >= subMesh->mStaticVertices.size())
                                        throw std::invalid_argument("Navigation mesh index out of range.");
                                    points[j] = glm::vec3(transform * glm::vec4(subMesh->mStaticVertices[indices[i + j]].Pos, 1));
                                }
                                bool interior = true;
                                for (const auto& point : points)
                                    for (int axis = 0; axis < 3; ++axis)
                                    {
                                        if (!std::isfinite(point[axis])) throw std::invalid_argument("Nonfinite navigation vertex.");
                                        interior &= point[axis] >= bounds->Minimum[axis] && point[axis] <= bounds->Maximum[axis];
                                    }
                                if (!interior)
                                {
                                    geometry.AddClippedTriangle(points[0], points[mirrored ? 2 : 1], points[mirrored ? 1 : 2],
                                        static_cast<unsigned char>(area), *bounds);
                                    continue;
                                }
                                const auto normal = glm::cross(points[1] - points[0], points[2] - points[0]);
                                if (glm::dot(normal, normal) < 1e-12f) continue;
                                if (tris.size() / 3 >= 1000000)
                                    throw std::invalid_argument("Navigation geometry exceeds one million vertices/triangles.");
                                int triangle[3];
                                for (int j = 0; j < 3; ++j)
                                {
                                    const std::array<float, 3> key{points[j].x, points[j].y, points[j].z};
                                    auto found = positions.find(key);
                                    if (found == positions.end())
                                    {
                                        if (verts.size() / 3 >= 1000000)
                                            throw std::invalid_argument("Navigation geometry exceeds one million vertices/triangles.");
                                        const int index = static_cast<int>(verts.size() / 3);
                                        verts.insert(verts.end(), key.begin(), key.end());
                                        found = positions.emplace(key, index).first;
                                    }
                                    triangle[j] = found->second;
                                }
                                tris.insert(tris.end(), {triangle[0], triangle[mirrored ? 2 : 1], triangle[mirrored ? 1 : 2]});
                                geometry.Areas.push_back(static_cast<unsigned char>(area));
                            }
                        }
                        catch (const std::exception& error) { mLastError = error.what(); return false; }
                        continue;
					}
					if (verts.size() / 3 + subMesh->mStaticVertices.size() > 1000000 || tris.size() + subMesh->GetIndices().size() > 3000000)
					{ mLastError = "Navigation geometry exceeds one million vertices/triangles."; return false; }
					const int base = static_cast<int>(verts.size() / 3);
					for (const auto& vertex : subMesh->mStaticVertices)
					{
						const glm::vec3 world = glm::vec3(transform * glm::vec4(vertex.Pos, 1));
						verts.insert(verts.end(), {world.x, world.y, world.z});
					}
					const auto& indices = subMesh->GetIndices();
					if (indices.size() % 3 != 0) { mLastError = "Navigation requires triangle indices."; return false; }
					for (size_t i = 0; i < indices.size(); i += 3)
					{
						for (size_t j = 0; j < 3; ++j)
							if (indices[i + j] >= subMesh->mStaticVertices.size()) { mLastError = "Navigation mesh index out of range."; return false; }
						tris.insert(tris.end(), {base + static_cast<int>(indices[i]),
							base + static_cast<int>(indices[i + (mirrored ? 2 : 1)]), base + static_cast<int>(indices[i + (mirrored ? 1 : 2)])});
                        geometry.Areas.push_back(static_cast<unsigned char>(area));
					}
				}
				continue;
			}
			const glm::vec3 scale = level->GetWorldScale(entity);
			const glm::vec3 halfSize = glm::vec3(
				std::max(0.1f, std::abs(scale.x) * 0.5f),
				0.0f,
				std::max(0.1f, std::abs(scale.z) * 0.5f));
            if (bounds)
            {
                const auto center = level->GetWorldTranslation(entity);
                const glm::vec3 a(center.x - halfSize.x, center.y, center.z - halfSize.z);
                const glm::vec3 b(center.x + halfSize.x, center.y, center.z - halfSize.z);
                const glm::vec3 c(center.x + halfSize.x, center.y, center.z + halfSize.z);
                const glm::vec3 d(center.x - halfSize.x, center.y, center.z + halfSize.z);
                try { geometry.AddClippedTriangle(a, c, b, area, *bounds); geometry.AddClippedTriangle(a, d, c, area, *bounds); }
                catch (const std::exception& error) { mLastError = error.what(); return false; }
            }
            else
            {
			    AddQuadOnXZ(level->GetWorldTranslation(entity), halfSize, verts, tris);
                geometry.Areas.insert(geometry.Areas.end(), 2, static_cast<unsigned char>(area));
            }
		}

        return BuildGeometry(geometry, settings);
    }

    bool NavigationService::BuildGeometry(const NavigationGeometry& geometry, const NavigationBuildSettings& settings)
    {
        mLastError.clear();
        try { ValidateSettings(settings); }
        catch (const std::exception& error) { mLastError = error.what(); return false; }
        const auto validRange = [](float value, float minimum, float maximum) {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        };
        if (!validRange(settings.CellSize, 0.01f, 10) || !validRange(settings.CellHeight, 0.01f, 10) ||
            !validRange(settings.AgentHeight, 0.1f, 100) || !validRange(settings.AgentRadius, 0, 100) ||
            !validRange(settings.AgentMaxClimb, 0, 100) || !validRange(settings.AgentMaxSlope, 0, 89) ||
            settings.MaxGridCells < 1 || settings.MaxGridCells > 16 * 1024 * 1024)
        { mLastError = "Invalid navigation agent settings or grid budget."; return false; }
        const auto& vertices = geometry.Vertices;
        const auto& indices = geometry.Indices;
        if (vertices.empty() || indices.empty() || vertices.size() % 3 || indices.size() % 3 ||
            vertices.size() > 3000000 || indices.size() > 3000000 ||
            (!geometry.Areas.empty() && geometry.Areas.size() != indices.size() / 3))
        { mLastError = "Navigation requires bounded world-space triangle geometry and matching optional areas."; return false; }
        for (float value : vertices)
            if (!std::isfinite(value) || std::abs(value) > 1000000)
            { mLastError = "Navigation vertex is nonfinite or exceeds coordinate limits."; return false; }
        for (int index : indices)
            if (index < 0 || static_cast<size_t>(index) >= vertices.size() / 3)
            { mLastError = "Navigation triangle index is out of range."; return false; }
        for (auto area : geometry.Areas)
            if (area > 63) { mLastError = "Navigation area must be 0..62 or blocked (63)."; return false; }
        NavigationService candidate;
        if (!candidate.BuildGeometryInternal(geometry, settings))
        {
            mLastError = candidate.mLastError.empty() ? "Recast could not build a traversable mesh; inspect geometry, clearance and slope." : candidate.mLastError;
            return false;
        }
        std::swap(mHeightfield, candidate.mHeightfield);
        std::swap(mCompactHeightfield, candidate.mCompactHeightfield);
        std::swap(mContourSet, candidate.mContourSet);
        std::swap(mPolyMesh, candidate.mPolyMesh);
        std::swap(mDetailMesh, candidate.mDetailMesh);
        std::swap(mNavMesh, candidate.mNavMesh);
        std::swap(mNavQuery, candidate.mNavQuery);
        mQuerySettings = {};
        if (!settings.AreaCosts.empty()) std::copy(settings.AreaCosts.begin(), settings.AreaCosts.end(), mQuerySettings.AreaCosts.begin());
        return true;
    }

    bool NavigationService::BuildGeometryInternal(const NavigationGeometry& geometry, const NavigationBuildSettings& settings)
    {
        const auto& verts = geometry.Vertices;
        const auto& tris = geometry.Indices;

        float bmin[3] = { verts[0], verts[1], verts[2] };
        float bmax[3] = { verts[0], verts[1], verts[2] };
        for (size_t i = 0; i < verts.size(); i += 3)
        {
            bmin[0] = std::min(bmin[0], verts[i + 0]);
            bmin[1] = std::min(bmin[1], verts[i + 1]);
            bmin[2] = std::min(bmin[2], verts[i + 2]);

            bmax[0] = std::max(bmax[0], verts[i + 0]);
            bmax[1] = std::max(bmax[1], verts[i + 1]);
            bmax[2] = std::max(bmax[2], verts[i + 2]);
        }

        const float cellSize = settings.CellSize;
        const float cellHeight = settings.CellHeight;
        const float agentHeight = settings.AgentHeight;
        const float agentRadius = settings.AgentRadius;
        const float agentMaxClimb = settings.AgentMaxClimb;
        const float agentMaxSlope = settings.AgentMaxSlope;
        // Give a flat floor enough vertical space for its spans and agent clearance.
        bmin[1] -= cellHeight;
        bmax[1] += agentHeight + cellHeight;
        const double width = std::ceil((double(bmax[0]) - bmin[0]) / cellSize);
        const double height = std::ceil((double(bmax[2]) - bmin[2]) / cellSize);
        if (width < 1 || height < 1 || width * height > settings.MaxGridCells)
        { mLastError = "Navigation grid exceeds the configured cell budget; reduce bounds or increase cell size."; return false; }

        rcConfig config{};
        config.cs = cellSize;
        config.ch = cellHeight;
        config.walkableSlopeAngle = agentMaxSlope;
        config.walkableHeight = static_cast<int>(ceilf(agentHeight / config.ch));
        config.walkableClimb = static_cast<int>(floorf(agentMaxClimb / config.ch));
        config.walkableRadius = static_cast<int>(ceilf(agentRadius / config.cs));
        config.maxEdgeLen = static_cast<int>(12.0f / cellSize);
        config.maxSimplificationError = 1.3f;
        config.minRegionArea = 8;
        config.mergeRegionArea = 20;
        config.maxVertsPerPoly = 6;
        config.detailSampleDist = 6.0f * config.cs;
        config.detailSampleMaxError = 1.0f * config.ch;

        rcVcopy(config.bmin, bmin);
        rcVcopy(config.bmax, bmax);
        rcCalcGridSize(config.bmin, config.bmax, config.cs, &config.width, &config.height);

        BuildContext ctx;

        mHeightfield = rcAllocHeightfield();
        if (!mHeightfield)
        {
            HE_CORE_WARN("NavigationService: rcAllocHeightfield failed");
            return false;
        }

        if (!rcCreateHeightfield(&ctx, *mHeightfield, config.width, config.height, config.bmin, config.bmax, config.cs, config.ch))
        {
            HE_CORE_WARN("NavigationService: rcCreateHeightfield failed");
            return false;
        }

        const int vertCount = static_cast<int>(verts.size() / 3);
        const int triCount = static_cast<int>(tris.size() / 3);
        std::vector<unsigned char> triAreas(static_cast<size_t>(triCount), 0);
        rcMarkWalkableTriangles(&ctx, config.walkableSlopeAngle, verts.data(), vertCount, tris.data(), triCount, triAreas.data());
        if (!geometry.Areas.empty())
            for (size_t i = 0; i < triAreas.size(); ++i)
                if (triAreas[i] != RC_NULL_AREA)
                    triAreas[i] = geometry.Areas[i] == 63 ? RC_NULL_AREA :
                        geometry.Areas[i] == 0 ? RC_WALKABLE_AREA : geometry.Areas[i];

        if (!rcRasterizeTriangles(&ctx, verts.data(), vertCount, tris.data(), triAreas.data(), triCount, *mHeightfield, config.walkableClimb))
        {
            HE_CORE_WARN("NavigationService: rcRasterizeTriangles failed");
            return false;
        }

        rcFilterLowHangingWalkableObstacles(&ctx, config.walkableClimb, *mHeightfield);
        rcFilterLedgeSpans(&ctx, config.walkableHeight, config.walkableClimb, *mHeightfield);
        rcFilterWalkableLowHeightSpans(&ctx, config.walkableHeight, *mHeightfield);

        mCompactHeightfield = rcAllocCompactHeightfield();
        if (!mCompactHeightfield)
        {
            HE_CORE_WARN("NavigationService: rcAllocCompactHeightfield failed");
            return false;
        }

        if (!rcBuildCompactHeightfield(&ctx, config.walkableHeight, config.walkableClimb, *mHeightfield, *mCompactHeightfield))
        {
            HE_CORE_WARN("NavigationService: rcBuildCompactHeightfield failed");
            return false;
        }

        if (!rcErodeWalkableArea(&ctx, config.walkableRadius, *mCompactHeightfield))
        {
            HE_CORE_WARN("NavigationService: rcErodeWalkableArea failed");
            return false;
        }

        if (!rcBuildDistanceField(&ctx, *mCompactHeightfield))
        {
            HE_CORE_WARN("NavigationService: rcBuildDistanceField failed");
            return false;
        }

        if (!rcBuildRegions(&ctx, *mCompactHeightfield, 0, config.minRegionArea, config.mergeRegionArea))
        {
            HE_CORE_WARN("NavigationService: rcBuildRegions failed");
            return false;
        }

        mContourSet = rcAllocContourSet();
        if (!mContourSet)
        {
            HE_CORE_WARN("NavigationService: rcAllocContourSet failed");
            return false;
        }

        if (!rcBuildContours(&ctx, *mCompactHeightfield, config.maxSimplificationError, config.maxEdgeLen, *mContourSet))
        {
            HE_CORE_WARN("NavigationService: rcBuildContours failed");
            return false;
        }

        mPolyMesh = rcAllocPolyMesh();
        if (!mPolyMesh)
        {
            HE_CORE_WARN("NavigationService: rcAllocPolyMesh failed");
            return false;
        }

        if (!rcBuildPolyMesh(&ctx, *mContourSet, config.maxVertsPerPoly, *mPolyMesh))
        {
            HE_CORE_WARN("NavigationService: rcBuildPolyMesh failed");
            return false;
        }

        if (mPolyMesh->npolys <= 0)
        {
            HE_CORE_WARN("NavigationService: rcBuildPolyMesh produced 0 polygons");
            return false;
        }

        mDetailMesh = rcAllocPolyMeshDetail();
        if (!mDetailMesh)
        {
            HE_CORE_WARN("NavigationService: rcAllocPolyMeshDetail failed");
            return false;
        }

        if (!rcBuildPolyMeshDetail(&ctx, *mPolyMesh, *mCompactHeightfield, config.detailSampleDist, config.detailSampleMaxError, *mDetailMesh))
        {
            HE_CORE_WARN("NavigationService: rcBuildPolyMeshDetail failed");
            return false;
        }

        if (config.maxVertsPerPoly > DT_VERTS_PER_POLYGON)
        {
            HE_CORE_WARN("NavigationService: config.maxVertsPerPoly exceeds DT_VERTS_PER_POLYGON");
            return false;
        }

        for (int i = 0; i < mPolyMesh->npolys; ++i)
        {
            if (mPolyMesh->areas[i] == RC_WALKABLE_AREA)
                mPolyMesh->areas[i] = 0;
            mPolyMesh->flags[i] = kWalkFlag;
        }

        dtNavMeshCreateParams params{};
        params.verts = mPolyMesh->verts;
        params.vertCount = mPolyMesh->nverts;
        params.polys = mPolyMesh->polys;
        params.polyAreas = mPolyMesh->areas;
        params.polyFlags = mPolyMesh->flags;
        params.polyCount = mPolyMesh->npolys;
        params.nvp = mPolyMesh->nvp;
        params.detailMeshes = mDetailMesh->meshes;
        params.detailVerts = mDetailMesh->verts;
        params.detailVertsCount = mDetailMesh->nverts;
        params.detailTris = mDetailMesh->tris;
        params.detailTriCount = mDetailMesh->ntris;
        params.walkableHeight = agentHeight;
        params.walkableRadius = agentRadius;
        params.walkableClimb = agentMaxClimb;
        rcVcopy(params.bmin, mPolyMesh->bmin);
        rcVcopy(params.bmax, mPolyMesh->bmax);
        params.cs = config.cs;
        params.ch = config.ch;
        params.buildBvTree = true;

        unsigned char* navData = nullptr;
        int navDataSize = 0;
        if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
        {
            HE_CORE_WARN("NavigationService: dtCreateNavMeshData failed");
            return false;
        }

        mNavMesh = dtAllocNavMesh();
        if (!mNavMesh)
        {
            HE_CORE_WARN("NavigationService: dtAllocNavMesh failed");
            dtFree(navData);
            return false;
        }

        dtStatus status = mNavMesh->init(navData, navDataSize, DT_TILE_FREE_DATA);
        if (dtStatusFailed(status))
        {
            HE_CORE_WARN("NavigationService: dtNavMesh::init failed");
            dtFree(navData);
            return false;
        }

        mNavQuery = dtAllocNavMeshQuery();
        if (!mNavQuery)
        {
            HE_CORE_WARN("NavigationService: dtAllocNavMeshQuery failed");
            return false;
        }

        status = mNavQuery->init(mNavMesh, 2048);
        if (dtStatusFailed(status))
        {
            HE_CORE_WARN("NavigationService: dtNavMeshQuery::init failed");
            return false;
        }

        HE_CORE_INFO("NavigationService: navmesh built successfully");
        return true;
    }

    bool NavigationService::FindPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& outPath)
    {
        return FindPath(start, end, outPath, mQuerySettings);
    }

    bool NavigationService::FindPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& outPath, const NavigationQuerySettings& settings)
    {
        outPath.clear();
        mLastError.clear();
        if (!mNavQuery)
        { mLastError = "No navigation mesh has been built."; return false; }
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(start[axis]) || !std::isfinite(end[axis]) || !std::isfinite(settings.SearchExtents[axis]) || settings.SearchExtents[axis] <= 0)
            { mLastError = "Navigation endpoints and positive search extents must be finite."; return false; }

        dtQueryFilter filter;
        filter.setIncludeFlags(kWalkFlag);
        filter.setExcludeFlags(0);
        for (size_t i = 0; i < settings.AreaCosts.size(); ++i)
        {
            const float cost = settings.AreaCosts[i];
            if (!std::isfinite(cost) || cost < 1 || cost > 1000000)
            { mLastError = "Navigation area costs must be finite and in [1,1000000]."; return false; }
            filter.setAreaCost(static_cast<int>(i), cost);
        }

        const float extents[3] = { settings.SearchExtents.x, settings.SearchExtents.y, settings.SearchExtents.z };
        const float startPos[3] = { start.x, start.y, start.z };
        const float endPos[3] = { end.x, end.y, end.z };

        dtPolyRef startRef = 0;
        dtPolyRef endRef = 0;
        float nearestStart[3] = { 0.0f, 0.0f, 0.0f };
        float nearestEnd[3] = { 0.0f, 0.0f, 0.0f };

        dtStatus status = mNavQuery->findNearestPoly(startPos, extents, &filter, &startRef, nearestStart);
        if (dtStatusFailed(status) || startRef == 0)
        { mLastError = "Start is outside the navigation search extent."; return false; }

        status = mNavQuery->findNearestPoly(endPos, extents, &filter, &endRef, nearestEnd);
        if (dtStatusFailed(status) || endRef == 0)
        { mLastError = "Destination is outside the navigation search extent."; return false; }

        constexpr int kMaxPolys = 256;
        dtPolyRef pathPolys[kMaxPolys] = {};
        int pathCount = 0;
        status = mNavQuery->findPath(startRef, endRef, nearestStart, nearestEnd, &filter, pathPolys, &pathCount, kMaxPolys);
        if (dtStatusFailed(status) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL | DT_OUT_OF_NODES | DT_PARTIAL_RESULT) ||
            pathCount <= 0 || pathPolys[pathCount - 1] != endRef)
        { mLastError = "Destination is unreachable or the path exceeds query limits."; return false; }

        constexpr int kMaxStraight = 256;
        float straightPath[kMaxStraight * 3] = {};
        unsigned char straightFlags[kMaxStraight] = {};
        dtPolyRef straightRefs[kMaxStraight] = {};
        int straightCount = 0;
        status = mNavQuery->findStraightPath(nearestStart, nearestEnd, pathPolys, pathCount, straightPath, straightFlags, straightRefs, &straightCount, kMaxStraight);
        if (dtStatusFailed(status) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL) || straightCount <= 0)
            return false;

        outPath.reserve(static_cast<size_t>(straightCount));
        for (int i = 0; i < straightCount; ++i)
        {
            outPath.emplace_back(straightPath[i * 3 + 0], straightPath[i * 3 + 1], straightPath[i * 3 + 2]);
        }

        return !outPath.empty();
    }

    std::vector<glm::vec3> NavigationService::GetDebugTriangles() const
    {
        std::vector<glm::vec3> result;
        if (!mPolyMesh) return result;
        const auto vertex = [&](unsigned short index) {
            const auto* v = &mPolyMesh->verts[index * 3];
            return glm::vec3(mPolyMesh->bmin[0] + v[0] * mPolyMesh->cs,
                mPolyMesh->bmin[1] + v[1] * mPolyMesh->ch, mPolyMesh->bmin[2] + v[2] * mPolyMesh->cs);
        };
        for (int i = 0; i < mPolyMesh->npolys; ++i)
        {
            const auto* poly = &mPolyMesh->polys[i * mPolyMesh->nvp * 2];
            for (int j = 2; j < mPolyMesh->nvp && poly[j] != RC_MESH_NULL_IDX; ++j)
                result.insert(result.end(), {vertex(poly[0]), vertex(poly[j - 1]), vertex(poly[j])});
        }
        return result;
    }

    void NavigationService::Release()
    {
        if (mNavQuery)
        {
            dtFreeNavMeshQuery(mNavQuery);
            mNavQuery = nullptr;
        }

        if (mNavMesh)
        {
            dtFreeNavMesh(mNavMesh);
            mNavMesh = nullptr;
        }

        if (mDetailMesh)
        {
            rcFreePolyMeshDetail(mDetailMesh);
            mDetailMesh = nullptr;
        }

        if (mPolyMesh)
        {
            rcFreePolyMesh(mPolyMesh);
            mPolyMesh = nullptr;
        }

        if (mContourSet)
        {
            rcFreeContourSet(mContourSet);
            mContourSet = nullptr;
        }

        if (mCompactHeightfield)
        {
            rcFreeCompactHeightfield(mCompactHeightfield);
            mCompactHeightfield = nullptr;
        }

        if (mHeightfield)
        {
            rcFreeHeightField(mHeightfield);
            mHeightfield = nullptr;
        }
    }

    bool NavigationService::IsReady() const
    {
        return mNavQuery != nullptr;
    }
}
