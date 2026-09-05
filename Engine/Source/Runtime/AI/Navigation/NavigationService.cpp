#include "Hepch.h"

#include "Runtime/AI/Navigation/NavigationService.h"

#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr unsigned short kWalkFlag = 0x01;

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

    NavigationService& NavigationService::Get()
    {
        static NavigationService sInstance;
        return sInstance;
    }

    bool NavigationService::BuildNavMesh(Level* level)
    {
        if (!level)
            return false;

        Release();

        std::vector<float> verts;
        std::vector<int> tris;

		auto view = level->mRegistry.view<TagComponent, TransformComponent>();
		for (auto handle : view)
		{
			Entity entity = { handle, level };
			const auto& tag = view.get<TagComponent>(handle);
			if (tag.Tag.rfind("NavGround", 0) != 0)
				continue;

			const glm::vec3 scale = level->GetWorldScale(entity);
			const glm::vec3 halfSize = glm::vec3(
				std::max(0.1f, std::abs(scale.x) * 0.5f),
				0.0f,
				std::max(0.1f, std::abs(scale.z) * 0.5f));
			AddQuadOnXZ(level->GetWorldTranslation(entity), halfSize, verts, tris);
		}

        if (tris.empty())
        {
            HE_CORE_WARN("NavigationService: no 'NavGround*' entities found, skip navmesh build");
            return false;
        }

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

        constexpr float cellSize = 0.2f;
        constexpr float cellHeight = 0.2f;
        constexpr float agentHeight = 1.8f;
        constexpr float agentRadius = 0.4f;
        constexpr float agentMaxClimb = 0.9f;
        constexpr float agentMaxSlope = 45.0f;

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
        outPath.clear();
        if (!mNavQuery)
            return false;

        dtQueryFilter filter;
        filter.setIncludeFlags(kWalkFlag);
        filter.setExcludeFlags(0);

        const float extents[3] = { 2.0f, 4.0f, 2.0f };
        const float startPos[3] = { start.x, start.y, start.z };
        const float endPos[3] = { end.x, end.y, end.z };

        dtPolyRef startRef = 0;
        dtPolyRef endRef = 0;
        float nearestStart[3] = { 0.0f, 0.0f, 0.0f };
        float nearestEnd[3] = { 0.0f, 0.0f, 0.0f };

        dtStatus status = mNavQuery->findNearestPoly(startPos, extents, &filter, &startRef, nearestStart);
        if (dtStatusFailed(status) || startRef == 0)
            return false;

        status = mNavQuery->findNearestPoly(endPos, extents, &filter, &endRef, nearestEnd);
        if (dtStatusFailed(status) || endRef == 0)
            return false;

        constexpr int kMaxPolys = 256;
        dtPolyRef pathPolys[kMaxPolys] = {};
        int pathCount = 0;
        status = mNavQuery->findPath(startRef, endRef, nearestStart, nearestEnd, &filter, pathPolys, &pathCount, kMaxPolys);
        if (dtStatusFailed(status) || pathCount <= 0)
            return false;

        constexpr int kMaxStraight = 256;
        float straightPath[kMaxStraight * 3] = {};
        unsigned char straightFlags[kMaxStraight] = {};
        dtPolyRef straightRefs[kMaxStraight] = {};
        int straightCount = 0;
        status = mNavQuery->findStraightPath(nearestStart, nearestEnd, pathPolys, pathCount, straightPath, straightFlags, straightRefs, &straightCount, kMaxStraight);
        if (dtStatusFailed(status) || straightCount <= 0)
            return false;

        outPath.reserve(static_cast<size_t>(straightCount));
        for (int i = 0; i < straightCount; ++i)
        {
            outPath.emplace_back(straightPath[i * 3 + 0], straightPath[i * 3 + 1], straightPath[i * 3 + 2]);
        }

        return !outPath.empty();
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
