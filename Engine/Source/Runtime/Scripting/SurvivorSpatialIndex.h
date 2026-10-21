#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    struct SurvivorSpatialEntry
    {
        uint32_t EntityId = 0;
        uint64_t ActivationId = 0;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        // QueryRadius tests entry centers; the radius is retained for later exact geometry batches.
        double Radius = 0.0;
        bool bActive = true;
        bool bDead = false;
    };

    struct SurvivorSpatialHit
    {
        uint32_t EntityId = 0;
        uint64_t ActivationId = 0;
        double DistanceSquared = 0.0;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        double Radius = 0.0;
    };

    struct SurvivorSpatialQueryStats
    {
        size_t VisitedCellCount = 0;
        size_t ExaminedEntryCount = 0;
    };

    class SurvivorSpatialIndex
    {
    public:
        explicit SurvivorSpatialIndex(double cellSize = 4.0);

        void Clear();
        void Rebuild(std::span<const SurvivorSpatialEntry> entries);

        SurvivorSpatialQueryStats QueryRadius(
            double originX,
            double originY,
            double originZ,
            double radius,
            double verticalRange,
            std::vector<SurvivorSpatialHit>& outHits) const;

        [[nodiscard]] size_t GetEntryCount() const { return mEntries.size(); }

    private:
        struct CellCoordinates
        {
            int64_t X = 0;
            int64_t Z = 0;

            bool operator==(const CellCoordinates&) const = default;
        };

        struct CellCoordinatesHash
        {
            [[nodiscard]] size_t operator()(const CellCoordinates& cell) const noexcept;
        };

        [[nodiscard]] CellCoordinates ToCell(double x, double z) const;

        double mCellSize = 4.0;
        std::vector<SurvivorSpatialEntry> mEntries;
        std::unordered_map<CellCoordinates, std::vector<size_t>, CellCoordinatesHash> mCells;
    };
}
