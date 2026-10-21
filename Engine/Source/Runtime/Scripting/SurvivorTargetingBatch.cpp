#include "Hepch.h"

#include "Runtime/Scripting/SurvivorTargetingBatch.h"
#include "Runtime/Core/TaskScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace HEngine
{
    namespace
    {
        constexpr uint64_t kHashSeed = 0x9e3779b97f4a7c15ULL;
        constexpr long double kMaximumGridCellsPerQuery = 1024.0L * 1024.0L;
        constexpr double kMinimumCapsuleSegmentLengthSquared = 0.000001;

        [[nodiscard]] uint64_t MixHash(uint64_t left, uint64_t right) noexcept
        {
            return left ^ (right + kHashSeed + (left << 6U) + (left >> 2U));
        }

        [[nodiscard]] bool StableKeyLess(
            const SurvivorTargetKey& left,
            const SurvivorTargetKey& right) noexcept
        {
            if (left.ActivationId != right.ActivationId)
                return left.ActivationId < right.ActivationId;
            return left.EntityId < right.EntityId;
        }

        [[nodiscard]] bool IsFiniteSample(const SurvivorTargetSample& sample)
        {
            return std::isfinite(sample.X) &&
                std::isfinite(sample.Y) &&
                std::isfinite(sample.Z) &&
                std::isfinite(sample.Radius) &&
                sample.Radius >= 0.0;
        }

        [[nodiscard]] bool IsFiniteNonNegative(double value)
        {
            return std::isfinite(value) && value >= 0.0;
        }

        [[nodiscard]] int64_t FloorToCell(double coordinate, double cellSize)
        {
            const double cell = std::floor(coordinate / cellSize);
            if (cell <= static_cast<double>(std::numeric_limits<int64_t>::min()))
                return std::numeric_limits<int64_t>::min();
            if (cell >= static_cast<double>(std::numeric_limits<int64_t>::max()))
                return std::numeric_limits<int64_t>::max();
            return static_cast<int64_t>(cell);
        }

        [[nodiscard]] long double InclusiveCellSpan(int64_t minimum, int64_t maximum)
        {
            return static_cast<long double>(maximum) -
                static_cast<long double>(minimum) + 1.0L;
        }

        [[nodiscard]] bool IsWithinHorizontalRadius(
            double deltaX,
            double deltaZ,
            double radius,
            double& outDistanceSquared)
        {
            outDistanceSquared = deltaX * deltaX + deltaZ * deltaZ;
            const double radiusSquared = radius * radius;
            if (std::isfinite(outDistanceSquared) && std::isfinite(radiusSquared))
                return outDistanceSquared <= radiusSquared;
            return std::hypot(deltaX, deltaZ) <= radius;
        }

        [[nodiscard]] bool IsStrictlyWithinHorizontalRadius(
            double deltaX,
            double deltaZ,
            double radius)
        {
            const double distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
            const double radiusSquared = radius * radius;
            if (std::isfinite(distanceSquared) && std::isfinite(radiusSquared))
                return distanceSquared < radiusSquared;
            return std::hypot(deltaX, deltaZ) < radius;
        }

        void MergeStats(
            SurvivorTargetingQueryStats& destination,
            const SurvivorTargetingQueryStats& source)
        {
            destination.VisitedCellCount += source.VisitedCellCount;
            destination.ExaminedSampleCount += source.ExaminedSampleCount;
            destination.GeometryTestCount += source.GeometryTestCount;
        }
    }

    size_t SurvivorTargetKeyHash::operator()(const SurvivorTargetKey& key) const noexcept
    {
        return static_cast<size_t>(MixHash(key.ActivationId, key.EntityId));
    }

    SurvivorTargetingBatch::SurvivorTargetingBatch(double cellSize)
        : mCellSize(cellSize)
    {
        if (!std::isfinite(cellSize) || cellSize <= 0.0)
            throw std::invalid_argument("Survivor targeting cell size must be finite and positive.");
    }

    void SurvivorTargetingBatch::Clear()
    {
        mMaximumTargetRadius = 0.0;
        mSamples.clear();
        mCells.clear();
        mCandidateIndexScratch.clear();
    }

    void SurvivorTargetingBatch::Rebuild(std::span<const SurvivorTargetSample> samples)
    {
        mMaximumTargetRadius = 0.0;
        mSamples.clear();
        for (auto& cell : mCells)
            cell.second.clear();
        mSamples.reserve(samples.size());
        mCells.reserve(samples.size());

        std::vector<uint8_t> valid;
        std::vector<CellCoordinates> cells;
        // Filtering and cell arithmetic are independent over the immutable
        // snapshot. Registry mutation and hash-table insertion stay on caller.
        if (samples.size() >= 32768)
        {
            valid.resize(samples.size());
            cells.resize(samples.size());
            (void)GetRuntimeTaskScheduler().ParallelFor(
                "Targeting snapshot cells", samples.size(), 2048,
                [&](size_t i)
                {
                    const auto& sample = samples[i];
                    valid[i] = sample.bActive && !sample.bDead && IsFiniteSample(sample);
                    if (valid[i]) cells[i] = ToCell(sample.X, sample.Z);
                });
        }
        for (size_t i = 0; i < samples.size(); ++i)
        {
            const SurvivorTargetSample& sample = samples[i];
            if (valid.empty()
                ? (!sample.bActive || sample.bDead || !IsFiniteSample(sample))
                : !valid[i])
                continue;

            const size_t sampleIndex = mSamples.size();
            mSamples.push_back(sample);
            mMaximumTargetRadius = std::max(mMaximumTargetRadius, sample.Radius);
            mCells[valid.empty() ? ToCell(sample.X, sample.Z) : cells[i]].push_back(sampleIndex);
        }

        const size_t maximumRetainedCellCount = std::max<size_t>(64, mSamples.size() * 4);
        if (mCells.size() > maximumRetainedCellCount)
        {
            std::erase_if(mCells, [](const auto& cell)
            {
                return cell.second.empty();
            });
        }
    }

    SurvivorTargetingQueryStats SurvivorTargetingBatch::QueryRadius(
        double originX,
        double originY,
        double originZ,
        double radius,
        double verticalRange,
        std::vector<SurvivorRadialTargetHit>& outHits) const
    {
        outHits.clear();
        SurvivorTargetingQueryStats stats;
        if (!std::isfinite(originX) ||
            !std::isfinite(originY) ||
            !std::isfinite(originZ) ||
            !IsFiniteNonNegative(radius) ||
            !IsFiniteNonNegative(verticalRange))
        {
            return stats;
        }

        std::vector<size_t>& candidateIndices = mCandidateIndexScratch;
        CollectCandidateIndices(
            originX - radius,
            originX + radius,
            originZ - radius,
            originZ + radius,
            candidateIndices,
            stats);

        for (const size_t sampleIndex : candidateIndices)
        {
            ++stats.GeometryTestCount;
            const SurvivorTargetSample& sample = mSamples[sampleIndex];
            if (std::abs(sample.Y - originY) > verticalRange)
                continue;

            double distanceSquared = 0.0;
            if (!IsWithinHorizontalRadius(
                sample.X - originX,
                sample.Z - originZ,
                radius,
                distanceSquared))
            {
                continue;
            }
            outHits.push_back({ sample, distanceSquared });
        }

        std::sort(outHits.begin(), outHits.end(), [](const auto& left, const auto& right)
        {
            return StableKeyLess(left.Target.Key, right.Target.Key);
        });
        return stats;
    }

    SurvivorTargetingQueryStats SurvivorTargetingBatch::SelectDensest(
        double originX,
        double originY,
        double originZ,
        double attackRange,
        double verticalRange,
        double clusterRadius,
        size_t targetCount,
        std::vector<SurvivorDensityTargetHit>& outTargets) const
    {
        outTargets.clear();
        SurvivorTargetingQueryStats stats;
        if (!IsFiniteNonNegative(clusterRadius) || targetCount == 0)
            return stats;

        std::vector<SurvivorRadialTargetHit> candidates;
        MergeStats(stats, QueryRadius(
            originX,
            originY,
            originZ,
            attackRange,
            verticalRange,
            candidates));
        if (candidates.empty())
            return stats;

        std::unordered_set<SurvivorTargetKey, SurvivorTargetKeyHash> candidateKeys;
        candidateKeys.reserve(candidates.size());
        for (const SurvivorRadialTargetHit& candidate : candidates)
            candidateKeys.insert(candidate.Target.Key);

        std::vector<size_t> neighborCounts(candidates.size(), 0);
        std::vector<size_t>& neighborIndices = mCandidateIndexScratch;
        for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
        {
            const SurvivorTargetSample& candidate = candidates[candidateIndex].Target;
            CollectCandidateIndices(
                candidate.X - clusterRadius,
                candidate.X + clusterRadius,
                candidate.Z - clusterRadius,
                candidate.Z + clusterRadius,
                neighborIndices,
                stats);
            for (const size_t neighborIndex : neighborIndices)
            {
                const SurvivorTargetSample& neighbor = mSamples[neighborIndex];
                if (!candidateKeys.contains(neighbor.Key))
                    continue;
                ++stats.GeometryTestCount;
                double ignoredDistanceSquared = 0.0;
                if (IsWithinHorizontalRadius(
                    neighbor.X - candidate.X,
                    neighbor.Z - candidate.Z,
                    clusterRadius,
                    ignoredDistanceSquared))
                {
                    ++neighborCounts[candidateIndex];
                }
            }
        }

        std::vector<bool> remaining(candidates.size(), true);
        outTargets.reserve(std::min(targetCount, candidates.size()));
        while (outTargets.size() < targetCount)
        {
            size_t bestIndex = candidates.size();
            size_t bestScore = 0;
            for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
            {
                if (!remaining[candidateIndex])
                    continue;
                if (bestIndex == candidates.size() || neighborCounts[candidateIndex] > bestScore)
                {
                    bestIndex = candidateIndex;
                    bestScore = neighborCounts[candidateIndex];
                }
            }
            if (bestIndex == candidates.size())
                break;

            const SurvivorRadialTargetHit& selected = candidates[bestIndex];
            outTargets.push_back({ selected.Target, selected.DistanceSquared, bestScore });
            remaining[bestIndex] = false;
            for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
            {
                if (!remaining[candidateIndex])
                    continue;
                ++stats.GeometryTestCount;
                const SurvivorTargetSample& candidate = candidates[candidateIndex].Target;
                if (IsStrictlyWithinHorizontalRadius(
                    candidate.X - selected.Target.X,
                    candidate.Z - selected.Target.Z,
                    clusterRadius))
                {
                    remaining[candidateIndex] = false;
                }
            }
        }
        return stats;
    }

    SurvivorTargetingQueryStats SurvivorTargetingBatch::QueryCorridors(
        double candidateOriginX,
        double candidateOriginY,
        double candidateOriginZ,
        double candidateRange,
        double candidateVerticalRange,
        std::span<const SurvivorCorridorQuery> corridors,
        std::vector<std::vector<SurvivorCorridorTargetHit>>& outHits) const
    {
        outHits.clear();
        outHits.resize(corridors.size());
        SurvivorTargetingQueryStats stats;

        std::vector<SurvivorRadialTargetHit> candidates;
        MergeStats(stats, QueryRadius(
            candidateOriginX,
            candidateOriginY,
            candidateOriginZ,
            candidateRange,
            candidateVerticalRange,
            candidates));

        for (size_t corridorIndex = 0; corridorIndex < corridors.size(); ++corridorIndex)
        {
            const SurvivorCorridorQuery& corridor = corridors[corridorIndex];
            std::vector<SurvivorCorridorTargetHit>& corridorHits = outHits[corridorIndex];
            if (!std::isfinite(corridor.OriginX) ||
                !std::isfinite(corridor.OriginZ) ||
                !std::isfinite(corridor.DirectionX) ||
                !std::isfinite(corridor.DirectionZ) ||
                !std::isfinite(corridor.MinimumAlong) ||
                !IsFiniteNonNegative(corridor.TravelDistance) ||
                !IsFiniteNonNegative(corridor.HalfWidth) ||
                corridor.MaximumHits == 0)
            {
                continue;
            }

            for (const SurvivorRadialTargetHit& candidate : candidates)
            {
                ++stats.GeometryTestCount;
                const double offsetX = candidate.Target.X - corridor.OriginX;
                const double offsetZ = candidate.Target.Z - corridor.OriginZ;
                const double along =
                    offsetX * corridor.DirectionX + offsetZ * corridor.DirectionZ;
                if (!std::isfinite(along) ||
                    along < corridor.MinimumAlong ||
                    along > corridor.TravelDistance)
                {
                    continue;
                }

                const double lateral = std::abs(
                    offsetX * corridor.DirectionZ - offsetZ * corridor.DirectionX);
                if (lateral <= corridor.HalfWidth + candidate.Target.Radius)
                    corridorHits.push_back({ candidate.Target, along });
            }

            std::sort(corridorHits.begin(), corridorHits.end(), [](const auto& left, const auto& right)
            {
                if (left.Along != right.Along)
                    return left.Along < right.Along;
                return StableKeyLess(left.Target.Key, right.Target.Key);
            });
            if (corridorHits.size() > corridor.MaximumHits)
                corridorHits.resize(corridor.MaximumHits);
        }
        return stats;
    }

    SurvivorTargetingQueryStats SurvivorTargetingBatch::SelectChain(
        double originX,
        double originY,
        double originZ,
        double attackRange,
        double verticalRange,
        double linkRange,
        size_t maximumTargets,
        std::vector<SurvivorRadialTargetHit>& outTargets) const
    {
        outTargets.clear();
        SurvivorTargetingQueryStats stats;
        if (!IsFiniteNonNegative(linkRange) || maximumTargets == 0)
            return stats;

        std::vector<SurvivorRadialTargetHit> candidates;
        MergeStats(stats, QueryRadius(
            originX,
            originY,
            originZ,
            attackRange,
            verticalRange,
            candidates));
        if (candidates.empty())
            return stats;

        size_t currentIndex = candidates.size();
        double currentDistanceSquared = 0.0;
        for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
        {
            if (currentIndex == candidates.size() ||
                candidates[candidateIndex].DistanceSquared < currentDistanceSquared)
            {
                currentIndex = candidateIndex;
                currentDistanceSquared = candidates[candidateIndex].DistanceSquared;
            }
        }

        std::vector<bool> remaining(candidates.size(), true);
        outTargets.reserve(std::min(maximumTargets, candidates.size()));
        while (currentIndex != candidates.size() && outTargets.size() < maximumTargets)
        {
            outTargets.push_back({ candidates[currentIndex].Target, currentDistanceSquared });
            remaining[currentIndex] = false;

            const SurvivorTargetSample& current = candidates[currentIndex].Target;
            size_t nextIndex = candidates.size();
            double nextDistanceSquared = 0.0;
            for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
            {
                if (!remaining[candidateIndex])
                    continue;
                ++stats.GeometryTestCount;

                const SurvivorTargetSample& candidate = candidates[candidateIndex].Target;
                double distanceSquared = 0.0;
                if (!IsWithinHorizontalRadius(
                    candidate.X - current.X,
                    candidate.Z - current.Z,
                    linkRange,
                    distanceSquared))
                {
                    continue;
                }
                if (nextIndex == candidates.size() || distanceSquared < nextDistanceSquared)
                {
                    nextIndex = candidateIndex;
                    nextDistanceSquared = distanceSquared;
                }
            }
            currentIndex = nextIndex;
            currentDistanceSquared = nextDistanceSquared;
        }
        return stats;
    }

    SurvivorTargetingQueryStats SurvivorTargetingBatch::QuerySweptAnnulus(
        double originX,
        double originY,
        double originZ,
        double previousRadius,
        double currentRadius,
        double edgeWidth,
        double verticalRange,
        std::vector<SurvivorRadialTargetHit>& outHits) const
    {
        outHits.clear();
        SurvivorTargetingQueryStats stats;
        if (!std::isfinite(originX) ||
            !std::isfinite(originY) ||
            !std::isfinite(originZ) ||
            !IsFiniteNonNegative(previousRadius) ||
            !IsFiniteNonNegative(currentRadius) ||
            !IsFiniteNonNegative(edgeWidth) ||
            !IsFiniteNonNegative(verticalRange))
        {
            return stats;
        }

        const double broadRadius =
            std::max(previousRadius, currentRadius) + edgeWidth + mMaximumTargetRadius;
        std::vector<size_t>& candidateIndices = mCandidateIndexScratch;
        CollectCandidateIndices(
            originX - broadRadius,
            originX + broadRadius,
            originZ - broadRadius,
            originZ + broadRadius,
            candidateIndices,
            stats);

        for (const size_t sampleIndex : candidateIndices)
        {
            ++stats.GeometryTestCount;
            const SurvivorTargetSample& sample = mSamples[sampleIndex];
            if (std::abs(sample.Y - originY) > verticalRange)
                continue;

            const double deltaX = sample.X - originX;
            const double deltaZ = sample.Z - originZ;
            const double distance = std::hypot(deltaX, deltaZ);
            const double sweptInner = std::max(
                0.0,
                previousRadius - edgeWidth - sample.Radius);
            const double sweptOuter = currentRadius + edgeWidth + sample.Radius;
            if (distance < sweptInner || distance > sweptOuter)
                continue;
            outHits.push_back({ sample, deltaX * deltaX + deltaZ * deltaZ });
        }

        std::sort(outHits.begin(), outHits.end(), [](const auto& left, const auto& right)
        {
            return StableKeyLess(left.Target.Key, right.Target.Key);
        });
        return stats;
    }

    SurvivorTargetingQueryStats SurvivorTargetingBatch::QueryCircleOverlaps(
        double originX,
        double originY,
        double originZ,
        double padding,
        double verticalRange,
        std::vector<SurvivorRadialTargetHit>& outHits) const
    {
        outHits.clear();
        SurvivorTargetingQueryStats stats;
        if (!std::isfinite(originX) ||
            !std::isfinite(originY) ||
            !std::isfinite(originZ) ||
            !IsFiniteNonNegative(padding) ||
            !IsFiniteNonNegative(verticalRange))
        {
            return stats;
        }

        const double broadRadius = padding + mMaximumTargetRadius;
        std::vector<size_t>& candidateIndices = mCandidateIndexScratch;
        CollectCandidateIndices(
            originX - broadRadius,
            originX + broadRadius,
            originZ - broadRadius,
            originZ + broadRadius,
            candidateIndices,
            stats);

        for (const size_t sampleIndex : candidateIndices)
        {
            ++stats.GeometryTestCount;
            const SurvivorTargetSample& sample = mSamples[sampleIndex];
            if (std::abs(sample.Y - originY) > verticalRange)
                continue;

            double distanceSquared = 0.0;
            if (!IsWithinHorizontalRadius(
                sample.X - originX,
                sample.Z - originZ,
                padding + sample.Radius,
                distanceSquared))
            {
                continue;
            }
            outHits.push_back({ sample, distanceSquared });
        }

        std::sort(outHits.begin(), outHits.end(), [](const auto& left, const auto& right)
        {
            if (left.DistanceSquared != right.DistanceSquared)
                return left.DistanceSquared < right.DistanceSquared;
            return StableKeyLess(left.Target.Key, right.Target.Key);
        });
        return stats;
    }

    SurvivorTargetingQueryStats SurvivorTargetingBatch::QuerySweptCapsule(
        double startX,
        double startZ,
        double endX,
        double endZ,
        double sweepRadius,
        std::vector<SurvivorCapsuleTargetHit>& outHits) const
    {
        outHits.clear();
        SurvivorTargetingQueryStats stats;
        if (!std::isfinite(startX) ||
            !std::isfinite(startZ) ||
            !std::isfinite(endX) ||
            !std::isfinite(endZ) ||
            !IsFiniteNonNegative(sweepRadius))
        {
            return stats;
        }

        const double segmentX = endX - startX;
        const double segmentZ = endZ - startZ;
        const double segmentLengthSquared = segmentX * segmentX + segmentZ * segmentZ;
        if (!std::isfinite(segmentLengthSquared) ||
            segmentLengthSquared < kMinimumCapsuleSegmentLengthSquared)
        {
            return stats;
        }

        const double broadPadding = sweepRadius + mMaximumTargetRadius;
        std::vector<size_t>& candidateIndices = mCandidateIndexScratch;
        CollectCandidateIndices(
            std::min(startX, endX) - broadPadding,
            std::max(startX, endX) + broadPadding,
            std::min(startZ, endZ) - broadPadding,
            std::max(startZ, endZ) + broadPadding,
            candidateIndices,
            stats);

        for (const size_t sampleIndex : candidateIndices)
        {
            ++stats.GeometryTestCount;
            const SurvivorTargetSample& sample = mSamples[sampleIndex];
            const double toTargetX = sample.X - startX;
            const double toTargetZ = sample.Z - startZ;
            const double segmentFraction = std::clamp(
                (toTargetX * segmentX + toTargetZ * segmentZ) / segmentLengthSquared,
                0.0,
                1.0);
            const double closestX = startX + segmentX * segmentFraction;
            const double closestZ = startZ + segmentZ * segmentFraction;
            double ignoredDistanceSquared = 0.0;
            if (!IsWithinHorizontalRadius(
                sample.X - closestX,
                sample.Z - closestZ,
                sweepRadius + sample.Radius,
                ignoredDistanceSquared))
            {
                continue;
            }
            outHits.push_back({ sample, segmentFraction });
        }

        std::sort(outHits.begin(), outHits.end(), [](const auto& left, const auto& right)
        {
            if (left.SegmentFraction != right.SegmentFraction)
                return left.SegmentFraction < right.SegmentFraction;
            return StableKeyLess(left.Target.Key, right.Target.Key);
        });
        return stats;
    }

    size_t SurvivorTargetingBatch::CellCoordinatesHash::operator()(
        const CellCoordinates& cell) const noexcept
    {
        return static_cast<size_t>(MixHash(
            static_cast<uint64_t>(cell.X),
            static_cast<uint64_t>(cell.Z)));
    }

    SurvivorTargetingBatch::CellCoordinates SurvivorTargetingBatch::ToCell(
        double x,
        double z) const
    {
        return {
            FloorToCell(x, mCellSize),
            FloorToCell(z, mCellSize)
        };
    }

    void SurvivorTargetingBatch::CollectCandidateIndices(
        double minimumX,
        double maximumX,
        double minimumZ,
        double maximumZ,
        std::vector<size_t>& outIndices,
        SurvivorTargetingQueryStats& stats) const
    {
        outIndices.clear();
        if (std::isnan(minimumX) ||
            std::isnan(maximumX) ||
            std::isnan(minimumZ) ||
            std::isnan(maximumZ) ||
            minimumX > maximumX ||
            minimumZ > maximumZ)
        {
            return;
        }

        const bool bUseFullScanForInfiniteBounds =
            !std::isfinite(minimumX) ||
            !std::isfinite(maximumX) ||
            !std::isfinite(minimumZ) ||
            !std::isfinite(maximumZ);
        CellCoordinates minimumCell;
        CellCoordinates maximumCell;
        bool bUseFullScan = bUseFullScanForInfiniteBounds;
        if (!bUseFullScan)
        {
            minimumCell = ToCell(minimumX, minimumZ);
            maximumCell = ToCell(maximumX, maximumZ);
            const long double cellCountX = InclusiveCellSpan(minimumCell.X, maximumCell.X);
            const long double cellCountZ = InclusiveCellSpan(minimumCell.Z, maximumCell.Z);
            bUseFullScan =
                cellCountX > kMaximumGridCellsPerQuery ||
                cellCountZ > kMaximumGridCellsPerQuery ||
                cellCountX * cellCountZ > kMaximumGridCellsPerQuery;
        }

        if (bUseFullScan)
        {
            outIndices.resize(mSamples.size());
            std::iota(outIndices.begin(), outIndices.end(), size_t{ 0 });
            stats.ExaminedSampleCount += mSamples.size();
            return;
        }

        for (int64_t cellX = minimumCell.X;; ++cellX)
        {
            for (int64_t cellZ = minimumCell.Z;; ++cellZ)
            {
                ++stats.VisitedCellCount;
                const auto cell = mCells.find({ cellX, cellZ });
                if (cell != mCells.end())
                {
                    outIndices.insert(
                        outIndices.end(),
                        cell->second.begin(),
                        cell->second.end());
                    stats.ExaminedSampleCount += cell->second.size();
                }
                if (cellZ == maximumCell.Z)
                    break;
            }
            if (cellX == maximumCell.X)
                break;
        }
    }
}
