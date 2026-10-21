#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    struct SurvivorTargetKey
    {
        uint32_t EntityId = 0;
        uint64_t ActivationId = 0;

        bool operator==(const SurvivorTargetKey&) const = default;
    };

    struct SurvivorTargetKeyHash
    {
        [[nodiscard]] size_t operator()(const SurvivorTargetKey& key) const noexcept;
    };

    // Active/dead flags let callers rebuild directly from their authoritative registry.
    // Valid samples have finite coordinates, a finite non-negative radius, and a unique key.
    struct SurvivorTargetSample
    {
        SurvivorTargetKey Key;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        double Radius = 0.0;
        bool bActive = true;
        bool bDead = false;
    };

    struct SurvivorRadialTargetHit
    {
        SurvivorTargetSample Target;
        double DistanceSquared = 0.0;
    };

    struct SurvivorDensityTargetHit
    {
        SurvivorTargetSample Target;
        double DistanceSquared = 0.0;
        size_t NeighborCount = 0;
    };

    struct SurvivorCorridorQuery
    {
        double OriginX = 0.0;
        double OriginZ = 0.0;
        double DirectionX = 0.0;
        double DirectionZ = 0.0;
        double MinimumAlong = 0.12;
        double TravelDistance = 0.0;
        double HalfWidth = 0.0;
        size_t MaximumHits = 0;
    };

    struct SurvivorCorridorTargetHit
    {
        SurvivorTargetSample Target;
        double Along = 0.0;
    };

    struct SurvivorCapsuleTargetHit
    {
        SurvivorTargetSample Target;
        double SegmentFraction = 0.0;
    };

    struct SurvivorTargetingQueryStats
    {
        size_t VisitedCellCount = 0;
        size_t ExaminedSampleCount = 0;
        size_t GeometryTestCount = 0;
    };

    // Pure gameplay geometry. The class owns an immutable snapshot between Rebuild calls;
    // no ECS, Python, renderer, or glm types cross this boundary. Stable target order is
    // ActivationId then EntityId. Corridor directions are consumed exactly as supplied;
    // gameplay callers provide the same normalized directions used by projectile motion.
    class SurvivorTargetingBatch
    {
    public:
        explicit SurvivorTargetingBatch(double cellSize = 4.0);

        void Clear();
        void Rebuild(std::span<const SurvivorTargetSample> samples);

        SurvivorTargetingQueryStats QueryRadius(
            double originX,
            double originY,
            double originZ,
            double radius,
            double verticalRange,
            std::vector<SurvivorRadialTargetHit>& outHits) const;

        SurvivorTargetingQueryStats SelectDensest(
            double originX,
            double originY,
            double originZ,
            double attackRange,
            double verticalRange,
            double clusterRadius,
            size_t targetCount,
            std::vector<SurvivorDensityTargetHit>& outTargets) const;

        SurvivorTargetingQueryStats QueryCorridors(
            double candidateOriginX,
            double candidateOriginY,
            double candidateOriginZ,
            double candidateRange,
            double candidateVerticalRange,
            std::span<const SurvivorCorridorQuery> corridors,
            std::vector<std::vector<SurvivorCorridorTargetHit>>& outHits) const;

        // DistanceSquared is measured from the initial origin for the first target and
        // from the previous selected target for every later target.
        SurvivorTargetingQueryStats SelectChain(
            double originX,
            double originY,
            double originZ,
            double attackRange,
            double verticalRange,
            double linkRange,
            size_t maximumTargets,
            std::vector<SurvivorRadialTargetHit>& outTargets) const;

        SurvivorTargetingQueryStats QuerySweptAnnulus(
            double originX,
            double originY,
            double originZ,
            double previousRadius,
            double currentRadius,
            double edgeWidth,
            double verticalRange,
            std::vector<SurvivorRadialTargetHit>& outHits) const;

        // Circle overlap expands the query padding by each target's own radius and
        // returns nearest first, with generation key as the deterministic tie-breaker.
        SurvivorTargetingQueryStats QueryCircleOverlaps(
            double originX,
            double originY,
            double originZ,
            double padding,
            double verticalRange,
            std::vector<SurvivorRadialTargetHit>& outHits) const;

        // This is a planar swept capsule. It deliberately has no vertical test because
        // star-wheel eligibility is fixed by the cast-time radius/vertical query.
        SurvivorTargetingQueryStats QuerySweptCapsule(
            double startX,
            double startZ,
            double endX,
            double endZ,
            double sweepRadius,
            std::vector<SurvivorCapsuleTargetHit>& outHits) const;

        [[nodiscard]] size_t GetSampleCount() const { return mSamples.size(); }
        [[nodiscard]] double GetMaximumTargetRadius() const { return mMaximumTargetRadius; }

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
        void CollectCandidateIndices(
            double minimumX,
            double maximumX,
            double minimumZ,
            double maximumZ,
            std::vector<size_t>& outIndices,
            SurvivorTargetingQueryStats& stats) const;

        double mCellSize = 4.0;
        double mMaximumTargetRadius = 0.0;
        std::vector<SurvivorTargetSample> mSamples;
        std::unordered_map<CellCoordinates, std::vector<size_t>, CellCoordinatesHash> mCells;
        // Main-thread queries are serialized by the Python gameplay bridge. Reuse
        // broad-phase storage so per-projectile capsule queries do not allocate.
        mutable std::vector<size_t> mCandidateIndexScratch;
    };
}
