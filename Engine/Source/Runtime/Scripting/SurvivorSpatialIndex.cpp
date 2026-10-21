#include "Hepch.h"

#include "Runtime/Scripting/SurvivorSpatialIndex.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        constexpr uint64_t kHashSeed = 0x9e3779b97f4a7c15ULL;
        constexpr uint64_t kMaximumGridCellsPerQuery = 1024ULL * 1024ULL;

        [[nodiscard]] int64_t FloorToCell(double coordinate, double cellSize)
        {
            const double cell = std::floor(coordinate / cellSize);
            if (cell <= static_cast<double>(std::numeric_limits<int64_t>::min()))
                return std::numeric_limits<int64_t>::min();
            if (cell >= static_cast<double>(std::numeric_limits<int64_t>::max()))
                return std::numeric_limits<int64_t>::max();
            return static_cast<int64_t>(cell);
        }

        [[nodiscard]] bool IsFiniteEntry(const SurvivorSpatialEntry& entry)
        {
            return std::isfinite(entry.X) &&
                std::isfinite(entry.Y) &&
                std::isfinite(entry.Z) &&
                std::isfinite(entry.Radius);
        }

        [[nodiscard]] long double InclusiveCellSpan(int64_t minimum, int64_t maximum)
        {
            return static_cast<long double>(maximum) -
                static_cast<long double>(minimum) + 1.0L;
        }

        [[nodiscard]] bool IsWithinRadius(
            const SurvivorSpatialEntry& entry,
            double originX,
            double originY,
            double originZ,
            double radius,
            double radiusSquared,
            double verticalRange,
            SurvivorSpatialHit& outHit)
        {
            if (std::abs(entry.Y - originY) > verticalRange)
                return false;

            const double deltaX = entry.X - originX;
            const double deltaZ = entry.Z - originZ;
            const double distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
            if (std::isfinite(distanceSquared) && std::isfinite(radiusSquared))
            {
                if (distanceSquared > radiusSquared)
                    return false;
            }
            else if (std::hypot(deltaX, deltaZ) > radius)
            {
                return false;
            }

            outHit.EntityId = entry.EntityId;
            outHit.ActivationId = entry.ActivationId;
            outHit.DistanceSquared = distanceSquared;
            outHit.X = entry.X;
            outHit.Y = entry.Y;
            outHit.Z = entry.Z;
            outHit.Radius = entry.Radius;
            return true;
        }
    }

    SurvivorSpatialIndex::SurvivorSpatialIndex(double cellSize)
        : mCellSize(cellSize)
    {
        if (!std::isfinite(cellSize) || cellSize <= 0.0)
            throw std::invalid_argument("Survivor spatial cell size must be finite and positive.");
    }

    void SurvivorSpatialIndex::Clear()
    {
        mEntries.clear();
        mCells.clear();
    }

    void SurvivorSpatialIndex::Rebuild(std::span<const SurvivorSpatialEntry> entries)
    {
        mEntries.clear();
        for (auto& cell : mCells)
            cell.second.clear();
        mEntries.reserve(entries.size());
        mCells.reserve(entries.size());

        for (const SurvivorSpatialEntry& entry : entries)
        {
            if (!entry.bActive || entry.bDead || !IsFiniteEntry(entry))
                continue;

            const size_t entryIndex = mEntries.size();
            mEntries.push_back(entry);
            mCells[ToCell(entry.X, entry.Z)].push_back(entryIndex);
        }

        const size_t maximumRetainedCellCount = std::max<size_t>(64, mEntries.size() * 4);
        if (mCells.size() > maximumRetainedCellCount)
        {
            std::erase_if(mCells, [](const auto& cell)
            {
                return cell.second.empty();
            });
        }
    }

    SurvivorSpatialQueryStats SurvivorSpatialIndex::QueryRadius(
        double originX,
        double originY,
        double originZ,
        double radius,
        double verticalRange,
        std::vector<SurvivorSpatialHit>& outHits) const
    {
        outHits.clear();
        SurvivorSpatialQueryStats stats;
        if (!std::isfinite(originX) ||
            !std::isfinite(originY) ||
            !std::isfinite(originZ) ||
            !std::isfinite(radius) ||
            !std::isfinite(verticalRange) ||
            radius < 0.0 ||
            verticalRange < 0.0)
        {
            return stats;
        }

        const double radiusSquared = radius * radius;
        const CellCoordinates minimumCell = ToCell(originX - radius, originZ - radius);
        const CellCoordinates maximumCell = ToCell(originX + radius, originZ + radius);
        const long double cellCountX = InclusiveCellSpan(minimumCell.X, maximumCell.X);
        const long double cellCountZ = InclusiveCellSpan(minimumCell.Z, maximumCell.Z);
        const bool bUseFullScan =
            cellCountX > static_cast<long double>(kMaximumGridCellsPerQuery) ||
            cellCountZ > static_cast<long double>(kMaximumGridCellsPerQuery) ||
            cellCountX * cellCountZ > static_cast<long double>(kMaximumGridCellsPerQuery);

        const auto examineEntry = [&](const SurvivorSpatialEntry& entry)
        {
            ++stats.ExaminedEntryCount;
            SurvivorSpatialHit hit;
            if (IsWithinRadius(
                entry,
                originX,
                originY,
                originZ,
                radius,
                radiusSquared,
                verticalRange,
                hit))
            {
                outHits.push_back(hit);
            }
        };

        if (bUseFullScan)
        {
            for (const SurvivorSpatialEntry& entry : mEntries)
                examineEntry(entry);
        }
        else
        {
            for (int64_t cellX = minimumCell.X;; ++cellX)
            {
                for (int64_t cellZ = minimumCell.Z;; ++cellZ)
                {
                    ++stats.VisitedCellCount;
                    const auto cell = mCells.find({ cellX, cellZ });
                    if (cell != mCells.end())
                    {
                        for (const size_t entryIndex : cell->second)
                            examineEntry(mEntries[entryIndex]);
                    }
                    if (cellZ == maximumCell.Z)
                        break;
                }
                if (cellX == maximumCell.X)
                    break;
            }
        }

        std::sort(outHits.begin(), outHits.end(), [](const auto& left, const auto& right)
        {
            if (left.ActivationId != right.ActivationId)
                return left.ActivationId < right.ActivationId;
            return left.EntityId < right.EntityId;
        });
        return stats;
    }

    size_t SurvivorSpatialIndex::CellCoordinatesHash::operator()(
        const CellCoordinates& cell) const noexcept
    {
        const uint64_t x = static_cast<uint64_t>(cell.X);
        const uint64_t z = static_cast<uint64_t>(cell.Z);
        const uint64_t mixed = x ^ (z + kHashSeed + (x << 6U) + (x >> 2U));
        return static_cast<size_t>(mixed);
    }

    SurvivorSpatialIndex::CellCoordinates SurvivorSpatialIndex::ToCell(
        double x,
        double z) const
    {
        return {
            FloorToCell(x, mCellSize),
            FloorToCell(z, mCellSize)
        };
    }
}
