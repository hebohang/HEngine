#include "Hepch.h"

#include "Runtime/EcsFramework/System/Navigation/NavPathDebugSystem.h"

#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Camera/Camera.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

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

        void DrawMarkerCross(const glm::vec3& p, const glm::vec4& color, float size)
        {
            Renderer2D::DrawLine(p + glm::vec3(-size, 0.0f, 0.0f), p + glm::vec3(size, 0.0f, 0.0f), color);
            Renderer2D::DrawLine(p + glm::vec3(0.0f, -size, 0.0f), p + glm::vec3(0.0f, size, 0.0f), color);
            Renderer2D::DrawLine(p + glm::vec3(0.0f, 0.0f, -size), p + glm::vec3(0.0f, 0.0f, size), color);
        }
    }

    NavPathDebugSystem::~NavPathDebugSystem()
    {
        ReleaseDetour();
        ReleaseBuildData();
    }

    void NavPathDebugSystem::OnUpdateRuntime(Timestep ts)
    {
        (void)ts;

        if (!ModeManager::b3DMode)
            return;

        glm::vec3 startWorld(0.0f);
        glm::vec3 endWorld(0.0f);
        if (!FindEntityPositionByTag("NavStart", startWorld) || !FindEntityPositionByTag("NavEnd", endWorld))
            return;

        if (!mNavQuery)
        {
            const bool built = RebuildNavMeshFromScene();
            if (!built)
            {
                if (mLastBuildSucceeded)
                    HE_CORE_WARN("NavPathDebugSystem: failed to build navmesh");
                mLastBuildSucceeded = false;
                return;
            }

            HE_CORE_INFO("NavPathDebugSystem: recast navmesh built successfully");
        }

        Camera* mainCamera = nullptr;
        glm::mat4 cameraTransform(1.0f);
		{
			auto view = mLevel->mRegistry.view<TransformComponent, CameraComponent>();
			for (auto entity : view)
			{
				Entity cameraEntity = { entity, mLevel };
				auto& camera = view.get<CameraComponent>(entity);
				if (camera.Primary)
				{
					mainCamera = &camera.Camera;
					cameraTransform = mLevel->GetWorldTransform(cameraEntity);
					break;
				}
			}
        }

        if (!mainCamera)
            return;

        dtQueryFilter filter;
        filter.setIncludeFlags(kWalkFlag);
        filter.setExcludeFlags(0);

        const float extents[3] = { 2.0f, 4.0f, 2.0f };

        const float startPos[3] = { startWorld.x, startWorld.y, startWorld.z };
        const float endPos[3] = { endWorld.x, endWorld.y, endWorld.z };

        dtPolyRef startRef = 0;
        dtPolyRef endRef = 0;
        float nearestStart[3] = { 0.0f, 0.0f, 0.0f };
        float nearestEnd[3] = { 0.0f, 0.0f, 0.0f };

        dtStatus status = mNavQuery->findNearestPoly(startPos, extents, &filter, &startRef, nearestStart);
        if (dtStatusFailed(status) || startRef == 0)
            return;

        status = mNavQuery->findNearestPoly(endPos, extents, &filter, &endRef, nearestEnd);
        if (dtStatusFailed(status) || endRef == 0)
            return;

        constexpr int kMaxPolys = 256;
        dtPolyRef pathPolys[kMaxPolys] = {};
        int pathCount = 0;
        status = mNavQuery->findPath(startRef, endRef, nearestStart, nearestEnd, &filter, pathPolys, &pathCount, kMaxPolys);
        if (dtStatusFailed(status) || pathCount <= 0)
            return;

        constexpr int kMaxStraight = 256;
        float straightPath[kMaxStraight * 3] = {};
        unsigned char straightFlags[kMaxStraight] = {};
        dtPolyRef straightRefs[kMaxStraight] = {};
        int straightCount = 0;
        status = mNavQuery->findStraightPath(nearestStart, nearestEnd, pathPolys, pathCount, straightPath, straightFlags, straightRefs, &straightCount, kMaxStraight);
        if (dtStatusFailed(status) || straightCount <= 0)
            return;

        Renderer2D::BeginScene(*mainCamera, cameraTransform);

        const glm::vec3 startMarker = glm::vec3(nearestStart[0], nearestStart[1], nearestStart[2]);
        const glm::vec3 endMarker = glm::vec3(nearestEnd[0], nearestEnd[1], nearestEnd[2]);
        DrawMarkerCross(startMarker, glm::vec4(0.25f, 1.0f, 0.35f, 1.0f), 0.2f);
        DrawMarkerCross(endMarker, glm::vec4(1.0f, 0.35f, 0.25f, 1.0f), 0.2f);

        for (int i = 1; i < straightCount; ++i)
        {
            const glm::vec3 p0 = glm::vec3(straightPath[(i - 1) * 3 + 0], straightPath[(i - 1) * 3 + 1], straightPath[(i - 1) * 3 + 2]);
            const glm::vec3 p1 = glm::vec3(straightPath[i * 3 + 0], straightPath[i * 3 + 1], straightPath[i * 3 + 2]);
            Renderer2D::DrawLine(p0 + glm::vec3(0.0f, 0.06f, 0.0f), p1 + glm::vec3(0.0f, 0.06f, 0.0f), glm::vec4(0.18f, 0.78f, 1.0f, 1.0f));
        }

        Renderer2D::EndScene();
    }

    void NavPathDebugSystem::ReleaseBuildData()
    {
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

    void NavPathDebugSystem::ReleaseDetour()
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
    }

    bool NavPathDebugSystem::FindEntityPositionByTag(const char* tagName, glm::vec3& outPosition) const
    {
		auto view = mLevel->mRegistry.view<TagComponent, TransformComponent>();
		for (auto handle : view)
		{
			const auto& tag = view.get<TagComponent>(handle);
			if (tag.Tag == tagName)
			{
				outPosition = mLevel->GetWorldTranslation(Entity{ handle, mLevel });
				return true;
			}
		}

        return false;
    }

    bool NavPathDebugSystem::RebuildNavMeshFromScene()
    {
        auto ReportBuildError = [this](const char* message)
        {
            if (!mBuildErrorReported)
            {
                HE_CORE_WARN("NavPathDebugSystem: {}", message);
                mBuildErrorReported = true;
            }
        };

        ReleaseDetour();
        ReleaseBuildData();

        std::vector<float> verts;
        std::vector<int> tris;

		auto view = mLevel->mRegistry.view<TagComponent, TransformComponent>();
		for (auto handle : view)
		{
			Entity entity = { handle, mLevel };
			const auto& tag = view.get<TagComponent>(handle);
			if (tag.Tag.rfind("NavGround", 0) != 0)
				continue;

			const glm::vec3 scale = mLevel->GetWorldScale(entity);
			const glm::vec3 halfSize = glm::vec3(
				std::max(0.1f, std::abs(scale.x) * 0.5f),
				0.0f,
				std::max(0.1f, std::abs(scale.z) * 0.5f));
			AddQuadOnXZ(mLevel->GetWorldTranslation(entity), halfSize, verts, tris);
		}

        if (tris.empty())
        {
            ReportBuildError("no 'NavGround*' entities found, skip navmesh build");
            mLastBuildSucceeded = false;
            return false;
        }

        mBuildErrorReported = false;

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
            ReportBuildError("rcAllocHeightfield failed");
            return false;
        }

        if (!rcCreateHeightfield(&ctx, *mHeightfield, config.width, config.height, config.bmin, config.bmax, config.cs, config.ch))
        {
            ReportBuildError("rcCreateHeightfield failed");
            return false;
        }

        const int vertCount = static_cast<int>(verts.size() / 3);
        const int triCount = static_cast<int>(tris.size() / 3);
        std::vector<unsigned char> triAreas(static_cast<size_t>(triCount), 0);
        rcMarkWalkableTriangles(&ctx, config.walkableSlopeAngle, verts.data(), vertCount, tris.data(), triCount, triAreas.data());

        if (!rcRasterizeTriangles(&ctx, verts.data(), vertCount, tris.data(), triAreas.data(), triCount, *mHeightfield, config.walkableClimb))
        {
            ReportBuildError("rcRasterizeTriangles failed");
            return false;
        }

        rcFilterLowHangingWalkableObstacles(&ctx, config.walkableClimb, *mHeightfield);
        rcFilterLedgeSpans(&ctx, config.walkableHeight, config.walkableClimb, *mHeightfield);
        rcFilterWalkableLowHeightSpans(&ctx, config.walkableHeight, *mHeightfield);

        mCompactHeightfield = rcAllocCompactHeightfield();
        if (!mCompactHeightfield)
        {
            ReportBuildError("rcAllocCompactHeightfield failed");
            return false;
        }

        if (!rcBuildCompactHeightfield(&ctx, config.walkableHeight, config.walkableClimb, *mHeightfield, *mCompactHeightfield))
        {
            ReportBuildError("rcBuildCompactHeightfield failed");
            return false;
        }

        if (!rcErodeWalkableArea(&ctx, config.walkableRadius, *mCompactHeightfield))
        {
            ReportBuildError("rcErodeWalkableArea failed");
            return false;
        }

        if (!rcBuildDistanceField(&ctx, *mCompactHeightfield))
        {
            ReportBuildError("rcBuildDistanceField failed");
            return false;
        }

        if (!rcBuildRegions(&ctx, *mCompactHeightfield, 0, config.minRegionArea, config.mergeRegionArea))
        {
            ReportBuildError("rcBuildRegions failed");
            return false;
        }

        mContourSet = rcAllocContourSet();
        if (!mContourSet)
        {
            ReportBuildError("rcAllocContourSet failed");
            return false;
        }

        if (!rcBuildContours(&ctx, *mCompactHeightfield, config.maxSimplificationError, config.maxEdgeLen, *mContourSet))
        {
            ReportBuildError("rcBuildContours failed");
            return false;
        }

        mPolyMesh = rcAllocPolyMesh();
        if (!mPolyMesh)
        {
            ReportBuildError("rcAllocPolyMesh failed");
            return false;
        }

        if (!rcBuildPolyMesh(&ctx, *mContourSet, config.maxVertsPerPoly, *mPolyMesh))
        {
            ReportBuildError("rcBuildPolyMesh failed");
            return false;
        }

        if (mPolyMesh->npolys <= 0)
        {
            ReportBuildError("rcBuildPolyMesh produced 0 polygons");
            return false;
        }

        mDetailMesh = rcAllocPolyMeshDetail();
        if (!mDetailMesh)
        {
            ReportBuildError("rcAllocPolyMeshDetail failed");
            return false;
        }

        if (!rcBuildPolyMeshDetail(&ctx, *mPolyMesh, *mCompactHeightfield, config.detailSampleDist, config.detailSampleMaxError, *mDetailMesh))
        {
            ReportBuildError("rcBuildPolyMeshDetail failed");
            return false;
        }

        if (config.maxVertsPerPoly > DT_VERTS_PER_POLYGON)
        {
            ReportBuildError("config.maxVertsPerPoly exceeds DT_VERTS_PER_POLYGON");
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
            ReportBuildError("dtCreateNavMeshData failed");
            return false;
        }

        mNavMesh = dtAllocNavMesh();
        if (!mNavMesh)
        {
            ReportBuildError("dtAllocNavMesh failed");
            dtFree(navData);
            return false;
        }

        dtStatus status = mNavMesh->init(navData, navDataSize, DT_TILE_FREE_DATA);
        if (dtStatusFailed(status))
        {
            ReportBuildError("dtNavMesh::init failed");
            dtFree(navData);
            return false;
        }

        mNavQuery = dtAllocNavMeshQuery();
        if (!mNavQuery)
        {
            ReportBuildError("dtAllocNavMeshQuery failed");
            return false;
        }

        status = mNavQuery->init(mNavMesh, 2048);
        if (dtStatusFailed(status))
        {
            ReportBuildError("dtNavMeshQuery::init failed");
            return false;
        }

        mBuildErrorReported = false;
        return true;
    }

    bool NavPathDebugSystem::BuildAndDrawPath(const glm::vec3& startWorld, const glm::vec3& endWorld, EditorCamera& camera)
    {
        if (!mNavQuery)
            return false;

        dtQueryFilter filter;
        filter.setIncludeFlags(kWalkFlag);
        filter.setExcludeFlags(0);

        const float extents[3] = { 2.0f, 4.0f, 2.0f };

        const float startPos[3] = { startWorld.x, startWorld.y, startWorld.z };
        const float endPos[3] = { endWorld.x, endWorld.y, endWorld.z };

        dtPolyRef startRef = 0;
        dtPolyRef endRef = 0;
        float nearestStart[3] = { 0.0f, 0.0f, 0.0f };
        float nearestEnd[3] = { 0.0f, 0.0f, 0.0f };

        dtStatus status = mNavQuery->findNearestPoly(startPos, extents, &filter, &startRef, nearestStart);
        if (dtStatusFailed(status) || startRef == 0)
        {
            HE_CORE_WARN("NavPathDebugSystem: findNearestPoly failed for NavStart");
            return false;
        }

        status = mNavQuery->findNearestPoly(endPos, extents, &filter, &endRef, nearestEnd);
        if (dtStatusFailed(status) || endRef == 0)
        {
            HE_CORE_WARN("NavPathDebugSystem: findNearestPoly failed for NavEnd");
            return false;
        }

        constexpr int kMaxPolys = 256;
        dtPolyRef pathPolys[kMaxPolys] = {};
        int pathCount = 0;
        status = mNavQuery->findPath(startRef, endRef, nearestStart, nearestEnd, &filter, pathPolys, &pathCount, kMaxPolys);
        if (dtStatusFailed(status) || pathCount <= 0)
        {
            HE_CORE_WARN("NavPathDebugSystem: findPath failed or empty path");
            return false;
        }

        constexpr int kMaxStraight = 256;
        float straightPath[kMaxStraight * 3] = {};
        unsigned char straightFlags[kMaxStraight] = {};
        dtPolyRef straightRefs[kMaxStraight] = {};
        int straightCount = 0;
        status = mNavQuery->findStraightPath(nearestStart, nearestEnd, pathPolys, pathCount, straightPath, straightFlags, straightRefs, &straightCount, kMaxStraight);
        if (dtStatusFailed(status) || straightCount <= 0)
        {
            HE_CORE_WARN("NavPathDebugSystem: findStraightPath failed");
            return false;
        }

        Renderer2D::BeginScene(camera);

        const glm::vec3 startMarker = glm::vec3(nearestStart[0], nearestStart[1], nearestStart[2]);
        const glm::vec3 endMarker = glm::vec3(nearestEnd[0], nearestEnd[1], nearestEnd[2]);
        DrawMarkerCross(startMarker, glm::vec4(0.25f, 1.0f, 0.35f, 1.0f), 0.2f);
        DrawMarkerCross(endMarker, glm::vec4(1.0f, 0.35f, 0.25f, 1.0f), 0.2f);

        for (int i = 1; i < straightCount; ++i)
        {
            const glm::vec3 p0 = glm::vec3(straightPath[(i - 1) * 3 + 0], straightPath[(i - 1) * 3 + 1], straightPath[(i - 1) * 3 + 2]);
            const glm::vec3 p1 = glm::vec3(straightPath[i * 3 + 0], straightPath[i * 3 + 1], straightPath[i * 3 + 2]);
            Renderer2D::DrawLine(p0 + glm::vec3(0.0f, 0.06f, 0.0f), p1 + glm::vec3(0.0f, 0.06f, 0.0f), glm::vec4(0.18f, 0.78f, 1.0f, 1.0f));
        }

        Renderer2D::EndScene();
        return true;
    }

#if WITH_EDITOR
    void NavPathDebugSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        (void)ts;

        if (!ModeManager::b3DMode)
            return;

        glm::vec3 startWorld(0.0f);
        glm::vec3 endWorld(0.0f);
        if (!FindEntityPositionByTag("NavStart", startWorld) || !FindEntityPositionByTag("NavEnd", endWorld))
            return;

        if (!mNavQuery)
        {
            const bool built = RebuildNavMeshFromScene();
            if (!built)
            {
                if (!mBuildErrorReported)
                {
                    HE_CORE_WARN("NavPathDebugSystem: failed to build navmesh");
                    mBuildErrorReported = true;
                }
                mLastBuildSucceeded = false;
                return;
            }

            HE_CORE_INFO("NavPathDebugSystem: recast navmesh built successfully");
        }

        mLastBuildSucceeded = BuildAndDrawPath(startWorld, endWorld, camera);
        if (!mLastBuildSucceeded)
            mBuildErrorReported = false;
    }
#endif
}
