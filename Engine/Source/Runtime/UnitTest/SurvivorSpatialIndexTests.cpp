#include "Hepch.h"

#include "Runtime/UnitTest/SurvivorSpatialIndexTests.h"
#include "Runtime/Scripting/SurvivorSpatialIndex.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        SurvivorSpatialEntry MakeEntry(
            uint32_t entityId,
            uint64_t activationId,
            double x,
            double y,
            double z,
            bool bActive = true,
            bool bDead = false)
        {
            return {
                entityId,
                activationId,
                x,
                y,
                z,
                0.5,
                bActive,
                bDead
            };
        }

        void RequireEntityIds(
            const std::vector<SurvivorSpatialHit>& hits,
            std::initializer_list<uint32_t> expectedIds,
            std::string_view message)
        {
            Require(hits.size() == expectedIds.size(), message);
            size_t index = 0;
            for (const uint32_t expectedId : expectedIds)
            {
                Require(hits[index].EntityId == expectedId, message);
                ++index;
            }
        }

        std::vector<SurvivorSpatialHit> BruteForceQuery(
            std::span<const SurvivorSpatialEntry> entries,
            double originX,
            double originY,
            double originZ,
            double radius,
            double verticalRange)
        {
            std::vector<SurvivorSpatialHit> hits;
            const double radiusSquared = radius * radius;
            for (const SurvivorSpatialEntry& entry : entries)
            {
                if (!entry.bActive || entry.bDead ||
                    !std::isfinite(entry.X) ||
                    !std::isfinite(entry.Y) ||
                    !std::isfinite(entry.Z) ||
                    !std::isfinite(entry.Radius) ||
                    std::abs(entry.Y - originY) > verticalRange)
                {
                    continue;
                }

                const double deltaX = entry.X - originX;
                const double deltaZ = entry.Z - originZ;
                const double distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
                if (distanceSquared <= radiusSquared)
                    hits.push_back({ entry.EntityId, entry.ActivationId, distanceSquared });
            }

            std::stable_sort(hits.begin(), hits.end(), [](const auto& left, const auto& right)
            {
                if (left.ActivationId != right.ActivationId)
                    return left.ActivationId < right.ActivationId;
                return left.EntityId < right.EntityId;
            });
            return hits;
        }
    }

    void SurvivorSpatialIndexContract()
    {
        SurvivorSpatialIndex index(4.0);
        std::vector<SurvivorSpatialHit> hits;

        const SurvivorSpatialQueryStats emptyStats =
            index.QueryRadius(0.0, 0.0, 0.0, 4.0, 2.0, hits);
        Require(hits.empty(), "An empty spatial index returned a hit.");
        Require(emptyStats.ExaminedEntryCount == 0,
            "An empty spatial index examined an entry.");

        const std::vector<SurvivorSpatialEntry> negativeBoundaryEntries = {
            MakeEntry(101, 3, -4.10, 0.0, -4.0),
            MakeEntry(102, 1, -3.90, 0.0, -4.0),
            MakeEntry(103, 2, -3.60, 0.0, -4.0)
        };
        index.Rebuild(negativeBoundaryEntries);
        index.QueryRadius(-4.0, 0.0, -4.0, 0.15, 0.0, hits);
        RequireEntityIds(hits, { 102, 101 },
            "Negative-coordinate cell boundaries changed query membership or stable order.");

        const std::vector<SurvivorSpatialEntry> exactBoundaryEntries = {
            MakeEntry(201, 1, 3.0, 2.0, 4.0),
            MakeEntry(202, 2, 3.0, 2.000001, 4.0),
            MakeEntry(203, 3, 3.0, 0.0, 4.000001),
            MakeEntry(204, 4, 0.0, 0.0, 0.0, false, false),
            MakeEntry(205, 5, 0.0, 0.0, 0.0, true, true)
        };
        index.Rebuild(exactBoundaryEntries);
        index.QueryRadius(0.0, 0.0, 0.0, 5.0, 2.0, hits);
        RequireEntityIds(hits, { 201 },
            "Inclusive radius/vertical boundaries or inactive/dead filtering changed.");
        Require(hits.front().DistanceSquared == 25.0,
            "The exact horizontal boundary returned the wrong distance.");

        const std::vector<SurvivorSpatialEntry> stableOrderEntries = {
            MakeEntry(330, 30, 1.0, 0.0, 0.0),
            MakeEntry(310, 10, -1.0, 0.0, 0.0),
            MakeEntry(320, 20, 0.0, 0.0, 1.0)
        };
        index.Rebuild(stableOrderEntries);
        index.QueryRadius(0.0, 0.0, 0.0, 2.0, 1.0, hits);
        RequireEntityIds(hits, { 310, 320, 330 },
            "Spatial bucket iteration leaked into activation order.");

        const std::vector<SurvivorSpatialEntry> wideActivationEntries = {
            MakeEntry(342, 9007199254740994ULL, 1.0, 0.0, 0.0),
            MakeEntry(341, 9007199254740993ULL, -1.0, 0.0, 0.0)
        };
        index.Rebuild(wideActivationEntries);
        index.QueryRadius(0.0, 0.0, 0.0, 1.0, 0.0, hits);
        RequireEntityIds(hits, { 341, 342 },
            "64-bit activation order lost precision above the exact double range.");

        const std::vector<SurvivorSpatialEntry> oldGeneration = {
            MakeEntry(401, 40, 0.0, 0.0, 0.0),
            MakeEntry(402, 41, 1.0, 0.0, 0.0)
        };
        index.Rebuild(oldGeneration);
        const std::vector<SurvivorSpatialEntry> newGeneration = {
            MakeEntry(401, 90, 0.0, 0.0, 0.0)
        };
        index.Rebuild(newGeneration);
        index.QueryRadius(0.0, 0.0, 0.0, 0.0, 0.0, hits);
        RequireEntityIds(hits, { 401 },
            "Rebuild retained a removed entity or rejected a zero-radius hit.");
        Require(hits.front().ActivationId == 90,
            "Rebuild leaked an entity's previous activation generation.");

        std::vector<SurvivorSpatialEntry> spreadEntries;
        spreadEntries.reserve(320);
        for (uint32_t indexValue = 0; indexValue < 320; ++indexValue)
        {
            const double x = (static_cast<int>(indexValue % 20) - 10) * 5.5;
            const double y = (static_cast<int>(indexValue % 5) - 2) * 0.75;
            const double z = (static_cast<int>(indexValue / 20) - 8) * 5.25;
            const uint64_t activationId = 1000 + (indexValue * 73) % 320;
            spreadEntries.push_back(MakeEntry(
                10000 + indexValue,
                activationId,
                x,
                y,
                z));
        }
        index.Rebuild(spreadEntries);
        const SurvivorSpatialQueryStats spreadStats =
            index.QueryRadius(-3.25, 0.25, 4.5, 12.7, 1.2, hits);
        const std::vector<SurvivorSpatialHit> bruteForceHits = BruteForceQuery(
            spreadEntries,
            -3.25,
            0.25,
            4.5,
            12.7,
            1.2);
        Require(hits.size() == bruteForceHits.size(),
            "The 320-entry grid query disagreed with brute force.");
        std::unordered_set<uint32_t> uniqueEntityIds;
        for (size_t hitIndex = 0; hitIndex < hits.size(); ++hitIndex)
        {
            Require(hits[hitIndex].EntityId == bruteForceHits[hitIndex].EntityId &&
                hits[hitIndex].ActivationId == bruteForceHits[hitIndex].ActivationId &&
                hits[hitIndex].DistanceSquared == bruteForceHits[hitIndex].DistanceSquared,
                "The 320-entry grid query changed exact membership, distance, or order.");
            Require(uniqueEntityIds.insert(hits[hitIndex].EntityId).second,
                "A multi-cell query returned the same entry twice.");
        }
        Require(spreadStats.ExaminedEntryCount < spreadEntries.size() / 2,
            "The bounded grid query regressed to a full 320-entry scan.");

        const double maximumCoordinate = std::numeric_limits<double>::max();
        const std::vector<SurvivorSpatialEntry> saturatedCellEntry = {
            MakeEntry(450, 1, maximumCoordinate, 0.0, maximumCoordinate)
        };
        index.Rebuild(saturatedCellEntry);
        index.QueryRadius(
            maximumCoordinate,
            0.0,
            maximumCoordinate,
            0.0,
            0.0,
            hits);
        RequireEntityIds(hits, { 450 },
            "A saturated INT64_MAX grid cell did not terminate or return its exact hit.");

        const std::vector<SurvivorSpatialEntry> hugeSpanEntries = {
            MakeEntry(451, 1, maximumCoordinate * 0.5, 0.0, 0.0),
            MakeEntry(452, 2, maximumCoordinate * 0.5, 0.0, maximumCoordinate * 0.5)
        };
        index.Rebuild(hugeSpanEntries);
        const SurvivorSpatialQueryStats hugeSpanStats = index.QueryRadius(
            0.0,
            0.0,
            0.0,
            maximumCoordinate * 0.5,
            0.0,
            hits);
        RequireEntityIds(hits, { 451 },
            "The overflow-safe full scan changed extreme-radius membership.");
        Require(hugeSpanStats.VisitedCellCount == 0 &&
            hugeSpanStats.ExaminedEntryCount == hugeSpanEntries.size(),
            "A huge cell span did not use the bounded full-scan fallback.");

        hits.push_back({ 999, 999, 999.0 });
        index.QueryRadius(
            std::numeric_limits<double>::quiet_NaN(),
            0.0,
            0.0,
            1.0,
            1.0,
            hits);
        Require(hits.empty(), "A non-finite query reused stale output or returned a hit.");
        index.QueryRadius(0.0, 0.0, 0.0, -1.0, 1.0, hits);
        Require(hits.empty(), "A negative-radius query returned a hit.");

        const std::vector<SurvivorSpatialEntry> invalidEntries = {
            MakeEntry(501, 1, std::numeric_limits<double>::infinity(), 0.0, 0.0),
            MakeEntry(502, 2, 0.0, 0.0, 0.0)
        };
        index.Rebuild(invalidEntries);
        Require(index.GetEntryCount() == 1,
            "Rebuild retained a non-finite spatial entry.");
        index.Clear();
        Require(index.GetEntryCount() == 0,
            "Clear retained spatial entries.");
    }
}
