#include "Hepch.h"

#include "Runtime/UnitTest/SurvivorTargetingBatchTests.h"
#include "Runtime/Scripting/SurvivorTargetingBatch.h"

#include <algorithm>
#include <chrono>
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

        void RequireNear(double actual, double expected, std::string_view message)
        {
            if (std::abs(actual - expected) > 0.000000001)
                throw std::runtime_error(std::string(message));
        }

        SurvivorTargetSample MakeSample(
            uint32_t entityId,
            uint64_t activationId,
            double x,
            double y,
            double z,
            double radius = 0.5,
            bool bActive = true,
            bool bDead = false)
        {
            return {
                { entityId, activationId },
                x,
                y,
                z,
                radius,
                bActive,
                bDead
            };
        }

        [[nodiscard]] bool StableKeyLess(
            const SurvivorTargetKey& left,
            const SurvivorTargetKey& right)
        {
            if (left.ActivationId != right.ActivationId)
                return left.ActivationId < right.ActivationId;
            return left.EntityId < right.EntityId;
        }

        template<typename Hit>
        void RequireKeys(
            const std::vector<Hit>& hits,
            std::initializer_list<SurvivorTargetKey> expectedKeys,
            std::string_view message)
        {
            Require(hits.size() == expectedKeys.size(), message);
            size_t index = 0;
            for (const SurvivorTargetKey& expectedKey : expectedKeys)
            {
                Require(hits[index].Target.Key == expectedKey, message);
                ++index;
            }
        }

        std::vector<SurvivorRadialTargetHit> ReferenceRadius(
            std::span<const SurvivorTargetSample> samples,
            double originX,
            double originY,
            double originZ,
            double radius,
            double verticalRange)
        {
            std::vector<SurvivorRadialTargetHit> hits;
            const double radiusSquared = radius * radius;
            for (const SurvivorTargetSample& sample : samples)
            {
                if (!sample.bActive ||
                    sample.bDead ||
                    !std::isfinite(sample.X) ||
                    !std::isfinite(sample.Y) ||
                    !std::isfinite(sample.Z) ||
                    !std::isfinite(sample.Radius) ||
                    sample.Radius < 0.0 ||
                    std::abs(sample.Y - originY) > verticalRange)
                {
                    continue;
                }

                const double deltaX = sample.X - originX;
                const double deltaZ = sample.Z - originZ;
                const double distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
                if (distanceSquared <= radiusSquared)
                    hits.push_back({ sample, distanceSquared });
            }
            std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right)
            {
                return StableKeyLess(left.Target.Key, right.Target.Key);
            });
            return hits;
        }

        std::vector<SurvivorDensityTargetHit> ReferenceDensest(
            std::span<const SurvivorTargetSample> samples,
            double originX,
            double originY,
            double originZ,
            double attackRange,
            double verticalRange,
            double clusterRadius,
            size_t targetCount)
        {
            const std::vector<SurvivorRadialTargetHit> candidates = ReferenceRadius(
                samples,
                originX,
                originY,
                originZ,
                attackRange,
                verticalRange);
            const double clusterRadiusSquared = clusterRadius * clusterRadius;
            std::vector<size_t> scores(candidates.size(), 0);
            for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
            {
                for (const SurvivorRadialTargetHit& neighbor : candidates)
                {
                    const double deltaX = neighbor.Target.X - candidates[candidateIndex].Target.X;
                    const double deltaZ = neighbor.Target.Z - candidates[candidateIndex].Target.Z;
                    if (deltaX * deltaX + deltaZ * deltaZ <= clusterRadiusSquared)
                        ++scores[candidateIndex];
                }
            }

            std::vector<bool> remaining(candidates.size(), true);
            std::vector<SurvivorDensityTargetHit> selected;
            while (selected.size() < targetCount)
            {
                size_t bestIndex = candidates.size();
                size_t bestScore = 0;
                for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
                {
                    if (!remaining[candidateIndex])
                        continue;
                    if (bestIndex == candidates.size() || scores[candidateIndex] > bestScore)
                    {
                        bestIndex = candidateIndex;
                        bestScore = scores[candidateIndex];
                    }
                }
                if (bestIndex == candidates.size())
                    break;

                const SurvivorRadialTargetHit& target = candidates[bestIndex];
                selected.push_back({ target.Target, target.DistanceSquared, bestScore });
                remaining[bestIndex] = false;
                for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
                {
                    if (!remaining[candidateIndex])
                        continue;
                    const double deltaX = candidates[candidateIndex].Target.X - target.Target.X;
                    const double deltaZ = candidates[candidateIndex].Target.Z - target.Target.Z;
                    if (deltaX * deltaX + deltaZ * deltaZ < clusterRadiusSquared)
                        remaining[candidateIndex] = false;
                }
            }
            return selected;
        }

        std::vector<std::vector<SurvivorCorridorTargetHit>> ReferenceCorridors(
            std::span<const SurvivorTargetSample> samples,
            double candidateOriginX,
            double candidateOriginY,
            double candidateOriginZ,
            double candidateRange,
            double candidateVerticalRange,
            std::span<const SurvivorCorridorQuery> corridors)
        {
            const std::vector<SurvivorRadialTargetHit> candidates = ReferenceRadius(
                samples,
                candidateOriginX,
                candidateOriginY,
                candidateOriginZ,
                candidateRange,
                candidateVerticalRange);
            std::vector<std::vector<SurvivorCorridorTargetHit>> results(corridors.size());
            for (size_t corridorIndex = 0; corridorIndex < corridors.size(); ++corridorIndex)
            {
                const SurvivorCorridorQuery& corridor = corridors[corridorIndex];
                std::vector<SurvivorCorridorTargetHit>& hits = results[corridorIndex];
                for (const SurvivorRadialTargetHit& candidate : candidates)
                {
                    const double offsetX = candidate.Target.X - corridor.OriginX;
                    const double offsetZ = candidate.Target.Z - corridor.OriginZ;
                    const double along =
                        offsetX * corridor.DirectionX + offsetZ * corridor.DirectionZ;
                    if (along < corridor.MinimumAlong || along > corridor.TravelDistance)
                        continue;
                    const double lateral = std::abs(
                        offsetX * corridor.DirectionZ - offsetZ * corridor.DirectionX);
                    if (lateral <= corridor.HalfWidth + candidate.Target.Radius)
                        hits.push_back({ candidate.Target, along });
                }
                std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right)
                {
                    if (left.Along != right.Along)
                        return left.Along < right.Along;
                    return StableKeyLess(left.Target.Key, right.Target.Key);
                });
                if (hits.size() > corridor.MaximumHits)
                    hits.resize(corridor.MaximumHits);
            }
            return results;
        }

        std::vector<SurvivorRadialTargetHit> ReferenceChain(
            std::span<const SurvivorTargetSample> samples,
            double originX,
            double originY,
            double originZ,
            double attackRange,
            double verticalRange,
            double linkRange,
            size_t maximumTargets)
        {
            const std::vector<SurvivorRadialTargetHit> candidates = ReferenceRadius(
                samples,
                originX,
                originY,
                originZ,
                attackRange,
                verticalRange);
            if (candidates.empty() || maximumTargets == 0)
                return {};

            size_t currentIndex = 0;
            for (size_t candidateIndex = 1; candidateIndex < candidates.size(); ++candidateIndex)
            {
                if (candidates[candidateIndex].DistanceSquared <
                    candidates[currentIndex].DistanceSquared)
                {
                    currentIndex = candidateIndex;
                }
            }

            std::vector<bool> remaining(candidates.size(), true);
            std::vector<SurvivorRadialTargetHit> chain;
            double legDistanceSquared = candidates[currentIndex].DistanceSquared;
            const double linkRangeSquared = linkRange * linkRange;
            while (chain.size() < maximumTargets)
            {
                chain.push_back({ candidates[currentIndex].Target, legDistanceSquared });
                remaining[currentIndex] = false;

                size_t nextIndex = candidates.size();
                double nextDistanceSquared = 0.0;
                for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
                {
                    if (!remaining[candidateIndex])
                        continue;
                    const double deltaX =
                        candidates[candidateIndex].Target.X - candidates[currentIndex].Target.X;
                    const double deltaZ =
                        candidates[candidateIndex].Target.Z - candidates[currentIndex].Target.Z;
                    const double distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
                    if (distanceSquared > linkRangeSquared)
                        continue;
                    if (nextIndex == candidates.size() || distanceSquared < nextDistanceSquared)
                    {
                        nextIndex = candidateIndex;
                        nextDistanceSquared = distanceSquared;
                    }
                }
                if (nextIndex == candidates.size())
                    break;
                currentIndex = nextIndex;
                legDistanceSquared = nextDistanceSquared;
            }
            return chain;
        }

        std::vector<SurvivorRadialTargetHit> ReferenceSweptAnnulus(
            std::span<const SurvivorTargetSample> samples,
            double originX,
            double originY,
            double originZ,
            double previousRadius,
            double currentRadius,
            double edgeWidth,
            double verticalRange)
        {
            std::vector<SurvivorRadialTargetHit> hits;
            for (const SurvivorTargetSample& sample : samples)
            {
                if (!sample.bActive ||
                    sample.bDead ||
                    !std::isfinite(sample.X) ||
                    !std::isfinite(sample.Y) ||
                    !std::isfinite(sample.Z) ||
                    !std::isfinite(sample.Radius) ||
                    sample.Radius < 0.0 ||
                    std::abs(sample.Y - originY) > verticalRange)
                {
                    continue;
                }
                const double deltaX = sample.X - originX;
                const double deltaZ = sample.Z - originZ;
                const double distance = std::hypot(deltaX, deltaZ);
                const double inner = std::max(
                    0.0,
                    previousRadius - edgeWidth - sample.Radius);
                const double outer = currentRadius + edgeWidth + sample.Radius;
                if (distance >= inner && distance <= outer)
                    hits.push_back({ sample, deltaX * deltaX + deltaZ * deltaZ });
            }
            std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right)
            {
                return StableKeyLess(left.Target.Key, right.Target.Key);
            });
            return hits;
        }

        std::vector<SurvivorRadialTargetHit> ReferenceCircleOverlaps(
            std::span<const SurvivorTargetSample> samples,
            double originX,
            double originY,
            double originZ,
            double padding,
            double verticalRange)
        {
            std::vector<SurvivorRadialTargetHit> hits;
            for (const SurvivorTargetSample& sample : samples)
            {
                if (!sample.bActive ||
                    sample.bDead ||
                    !std::isfinite(sample.X) ||
                    !std::isfinite(sample.Y) ||
                    !std::isfinite(sample.Z) ||
                    !std::isfinite(sample.Radius) ||
                    sample.Radius < 0.0 ||
                    std::abs(sample.Y - originY) > verticalRange)
                {
                    continue;
                }
                const double deltaX = sample.X - originX;
                const double deltaZ = sample.Z - originZ;
                const double distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
                const double combinedRadius = padding + sample.Radius;
                if (distanceSquared <= combinedRadius * combinedRadius)
                    hits.push_back({ sample, distanceSquared });
            }
            std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right)
            {
                if (left.DistanceSquared != right.DistanceSquared)
                    return left.DistanceSquared < right.DistanceSquared;
                return StableKeyLess(left.Target.Key, right.Target.Key);
            });
            return hits;
        }

        std::vector<SurvivorCapsuleTargetHit> ReferenceSweptCapsule(
            std::span<const SurvivorTargetSample> samples,
            double startX,
            double startZ,
            double endX,
            double endZ,
            double sweepRadius)
        {
            const double segmentX = endX - startX;
            const double segmentZ = endZ - startZ;
            const double segmentLengthSquared = segmentX * segmentX + segmentZ * segmentZ;
            if (segmentLengthSquared < 0.000001)
                return {};

            std::vector<SurvivorCapsuleTargetHit> hits;
            for (const SurvivorTargetSample& sample : samples)
            {
                if (!sample.bActive ||
                    sample.bDead ||
                    !std::isfinite(sample.X) ||
                    !std::isfinite(sample.Y) ||
                    !std::isfinite(sample.Z) ||
                    !std::isfinite(sample.Radius) ||
                    sample.Radius < 0.0)
                {
                    continue;
                }
                const double toTargetX = sample.X - startX;
                const double toTargetZ = sample.Z - startZ;
                const double fraction = std::clamp(
                    (toTargetX * segmentX + toTargetZ * segmentZ) / segmentLengthSquared,
                    0.0,
                    1.0);
                const double closestX = startX + segmentX * fraction;
                const double closestZ = startZ + segmentZ * fraction;
                const double deltaX = sample.X - closestX;
                const double deltaZ = sample.Z - closestZ;
                const double combinedRadius = sweepRadius + sample.Radius;
                if (deltaX * deltaX + deltaZ * deltaZ <= combinedRadius * combinedRadius)
                    hits.push_back({ sample, fraction });
            }
            std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right)
            {
                if (left.SegmentFraction != right.SegmentFraction)
                    return left.SegmentFraction < right.SegmentFraction;
                return StableKeyLess(left.Target.Key, right.Target.Key);
            });
            return hits;
        }

        void RequireDensityMatchesReference(
            const std::vector<SurvivorDensityTargetHit>& actual,
            const std::vector<SurvivorDensityTargetHit>& expected,
            std::string_view message)
        {
            Require(actual.size() == expected.size(), message);
            for (size_t index = 0; index < actual.size(); ++index)
            {
                Require(actual[index].Target.Key == expected[index].Target.Key, message);
                Require(actual[index].NeighborCount == expected[index].NeighborCount, message);
                RequireNear(actual[index].DistanceSquared, expected[index].DistanceSquared, message);
            }
        }

        void RequireSampleMatches(
            const SurvivorTargetSample& actual,
            const SurvivorTargetSample& expected,
            std::string_view message)
        {
            Require(actual.Key == expected.Key, message);
            RequireNear(actual.X, expected.X, message);
            RequireNear(actual.Y, expected.Y, message);
            RequireNear(actual.Z, expected.Z, message);
            RequireNear(actual.Radius, expected.Radius, message);
        }

        void RequireRadialMatchesReference(
            const std::vector<SurvivorRadialTargetHit>& actual,
            const std::vector<SurvivorRadialTargetHit>& expected,
            std::string_view message)
        {
            Require(actual.size() == expected.size(), message);
            for (size_t index = 0; index < actual.size(); ++index)
            {
                RequireSampleMatches(actual[index].Target, expected[index].Target, message);
                RequireNear(actual[index].DistanceSquared, expected[index].DistanceSquared, message);
            }
        }

        void RequireCorridorsMatchReference(
            const std::vector<std::vector<SurvivorCorridorTargetHit>>& actual,
            const std::vector<std::vector<SurvivorCorridorTargetHit>>& expected,
            std::string_view message)
        {
            Require(actual.size() == expected.size(), message);
            for (size_t corridorIndex = 0; corridorIndex < actual.size(); ++corridorIndex)
            {
                Require(actual[corridorIndex].size() == expected[corridorIndex].size(), message);
                for (size_t hitIndex = 0; hitIndex < actual[corridorIndex].size(); ++hitIndex)
                {
                    RequireSampleMatches(
                        actual[corridorIndex][hitIndex].Target,
                        expected[corridorIndex][hitIndex].Target,
                        message);
                    RequireNear(
                        actual[corridorIndex][hitIndex].Along,
                        expected[corridorIndex][hitIndex].Along,
                        message);
                }
            }
        }

        void RequireCapsuleMatchesReference(
            const std::vector<SurvivorCapsuleTargetHit>& actual,
            const std::vector<SurvivorCapsuleTargetHit>& expected,
            std::string_view message)
        {
            Require(actual.size() == expected.size(), message);
            for (size_t index = 0; index < actual.size(); ++index)
            {
                RequireSampleMatches(actual[index].Target, expected[index].Target, message);
                RequireNear(actual[index].SegmentFraction, expected[index].SegmentFraction, message);
            }
        }

        void RadiusAndGenerationContract()
        {
            SurvivorTargetingBatch batch(4.0);
            std::vector<SurvivorRadialTargetHit> hits;
            const std::vector<SurvivorTargetSample> samples = {
                MakeSample(10, 30, 3.0, 2.0, 4.0),
                MakeSample(11, 10, -3.0, -2.0, -4.0),
                MakeSample(12, 20, 0.0, 2.000001, 0.0),
                MakeSample(13, 40, 0.0, 0.0, 0.0, 0.5, false, false),
                MakeSample(14, 50, 0.0, 0.0, 0.0, 0.5, true, true),
                MakeSample(15, 60, 0.0, 0.0, 0.0, -0.5),
                MakeSample(16, 70, std::numeric_limits<double>::infinity(), 0.0, 0.0)
            };
            batch.Rebuild(samples);
            batch.QueryRadius(0.0, 0.0, 0.0, 5.0, 2.0, hits);
            RequireKeys(hits, { { 11, 10 }, { 10, 30 } },
                "Radius membership, inclusive boundaries, or stable activation order changed.");
            RequireNear(hits[0].DistanceSquared, 25.0,
                "Radius query returned the wrong exact boundary distance.");
            Require(batch.GetSampleCount() == 3,
                "Rebuild retained inactive, dead, non-finite, or negative-radius samples.");

            const std::vector<SurvivorTargetSample> generationSamples = {
                MakeSample(42, 100, 0.0, 0.0, 0.0, 0.5, false, false),
                MakeSample(42, 101, 0.0, 0.0, 0.0)
            };
            batch.Rebuild(generationSamples);
            batch.QueryRadius(0.0, 0.0, 0.0, 0.0, 0.0, hits);
            RequireKeys(hits, { { 42, 101 } },
                "An inactive pooled generation leaked into targeting results.");

            std::unordered_set<SurvivorTargetKey, SurvivorTargetKeyHash> generations;
            generations.insert({ 42, 100 });
            generations.insert({ 42, 101 });
            Require(generations.size() == 2,
                "Target identity collapsed distinct activation generations.");

            hits.push_back({ MakeSample(99, 99, 0.0, 0.0, 0.0), 0.0 });
            batch.QueryRadius(
                std::numeric_limits<double>::quiet_NaN(),
                0.0,
                0.0,
                1.0,
                1.0,
                hits);
            Require(hits.empty(), "An invalid radius query retained stale output.");
        }

        void DensestContract()
        {
            SurvivorTargetingBatch batch(4.0);
            std::vector<SurvivorDensityTargetHit> selected;
            const std::vector<SurvivorTargetSample> exactSamples = {
                MakeSample(101, 1, 0.0, -2.0, 0.0),
                MakeSample(102, 2, 1.0, 2.0, 0.0),
                MakeSample(103, 3, 0.0, 0.0, 1.0),
                MakeSample(104, 4, 5.0, 0.0, 0.0),
                MakeSample(105, 5, 6.0, 0.0, 0.0),
                MakeSample(106, 6, 0.2, 2.000001, 0.2)
            };
            batch.Rebuild(exactSamples);
            batch.SelectDensest(0.0, 0.0, 0.0, 10.0, 2.0, 1.5, 2, selected);
            RequireKeys(selected, { { 101, 1 }, { 104, 4 } },
                "Densest selection changed full-set scoring, Y-independent clustering, or stable ties.");
            Require(selected[0].NeighborCount == 3 && selected[1].NeighborCount == 2,
                "Densest selection did not score against the complete original candidate set.");

            const std::vector<SurvivorTargetSample> equalitySamples = {
                MakeSample(201, 1, 0.0, 0.0, 0.0),
                MakeSample(202, 2, 2.0, 0.0, 0.0)
            };
            batch.Rebuild(equalitySamples);
            batch.SelectDensest(0.0, 0.0, 0.0, 10.0, 1.0, 2.0, 2, selected);
            RequireKeys(selected, { { 201, 1 }, { 202, 2 } },
                "A target exactly clusterRadius away was incorrectly removed by separation.");
            Require(selected[0].NeighborCount == 2 && selected[1].NeighborCount == 2,
                "Inclusive density membership changed at the exact cluster boundary.");

            std::vector<SurvivorTargetSample> spreadSamples;
            spreadSamples.reserve(320);
            for (uint32_t index = 0; index < 320; ++index)
            {
                const double x = (static_cast<int>(index % 20) - 10) * 5.5;
                const double y = (static_cast<int>(index % 5) - 2) * 0.75;
                const double z = (static_cast<int>(index / 20) - 8) * 5.25;
                spreadSamples.push_back(MakeSample(
                    10000 + index,
                    1000 + (index * 73) % 320,
                    x,
                    y,
                    z));
            }
            batch.Rebuild(spreadSamples);
            const SurvivorTargetingQueryStats stats = batch.SelectDensest(
                0.0,
                0.0,
                0.0,
                100.0,
                4.0,
                6.0,
                12,
                selected);
            const std::vector<SurvivorDensityTargetHit> reference = ReferenceDensest(
                spreadSamples,
                0.0,
                0.0,
                0.0,
                100.0,
                4.0,
                6.0,
                12);
            RequireDensityMatchesReference(selected, reference,
                "The 320-target grid density result disagreed with independent brute force.");
            Require(stats.GeometryTestCount < spreadSamples.size() * spreadSamples.size() / 2,
                "Spread density targeting regressed to an all-pairs geometry scan.");
        }

        void CorridorContract()
        {
            SurvivorTargetingBatch batch(4.0);
            const std::vector<SurvivorTargetSample> samples = {
                MakeSample(301, 4, 0.12, 0.0, 0.0),
                MakeSample(302, 1, 0.119, 0.0, 0.0),
                MakeSample(303, 5, 10.0, 0.0, 0.0),
                MakeSample(304, 3, 5.0, 0.0, 1.0),
                MakeSample(305, 2, 5.0, 0.0, 0.0),
                MakeSample(306, 6, 5.0, 0.0, 1.001),
                MakeSample(307, 7, 1.0, 0.0, 1.0),
                MakeSample(308, 8, 2.0, 1.000001, 0.0)
            };
            batch.Rebuild(samples);
            const std::vector<SurvivorCorridorQuery> corridors = {
                { 0.0, 0.0, 1.0, 0.0, 0.12, 10.0, 0.5, 10 },
                { 0.0, 0.0, 0.0, 1.0, 0.12, 10.0, 0.5, 10 },
                { 0.0, 0.0, 1.0, 0.0, 0.12, 10.0, 0.5, 2 }
            };
            std::vector<std::vector<SurvivorCorridorTargetHit>> hits;
            batch.QueryCorridors(
                0.0,
                0.0,
                0.0,
                20.0,
                1.0,
                corridors,
                hits);
            Require(hits.size() == 3, "Corridor batch changed output grouping.");
            RequireKeys(hits[0],
                { { 301, 4 }, { 307, 7 }, { 305, 2 }, { 304, 3 }, { 303, 5 } },
                "Corridor boundaries, along ordering, target radius, or stable tie changed.");
            RequireKeys(hits[1], { { 307, 7 } },
                "Corridor queries incorrectly deduplicated a target across rays.");
            RequireKeys(hits[2], { { 301, 4 }, { 307, 7 } },
                "Corridor MaximumHits was not applied independently per ray.");
            RequireNear(hits[0][0].Along, 0.12,
                "Corridor rejected or moved the inclusive minimum-along boundary.");
            RequireNear(hits[0].back().Along, 10.0,
                "Corridor rejected or moved the inclusive travel endpoint.");
        }

        void ChainContract()
        {
            SurvivorTargetingBatch batch(4.0);
            const std::vector<SurvivorTargetSample> samples = {
                MakeSample(401, 2, 1.0, 0.0, 0.0),
                MakeSample(402, 1, -1.0, 0.0, 0.0),
                MakeSample(403, 3, -2.5, 2.0, 0.0),
                MakeSample(404, 4, -4.0, -2.0, 0.0),
                MakeSample(405, 5, 10.0, 0.0, 0.0),
                MakeSample(406, 6, -2.0, 2.000001, 0.0)
            };
            batch.Rebuild(samples);
            std::vector<SurvivorRadialTargetHit> chain;
            batch.SelectChain(0.0, 0.0, 0.0, 20.0, 2.0, 1.5, 3, chain);
            RequireKeys(chain, { { 402, 1 }, { 403, 3 }, { 404, 4 } },
                "Chain selection changed nearest ties, inclusive links, planar links, or no-repeat behavior.");
            RequireNear(chain[0].DistanceSquared, 1.0,
                "The first chain metric was not measured from the cast origin.");
            RequireNear(chain[1].DistanceSquared, 2.25,
                "The second chain metric was not measured from the previous target.");
            RequireNear(chain[2].DistanceSquared, 2.25,
                "The exact link boundary changed on a later bounce.");

            batch.SelectChain(0.0, 0.0, 0.0, 20.0, 2.0, 0.1, 6, chain);
            Require(chain.size() == 1,
                "A chain retargeted after no remaining target was within link range.");
        }

        void AnnulusContract()
        {
            SurvivorTargetingBatch batch(4.0);
            const std::vector<SurvivorTargetSample> samples = {
                MakeSample(501, 1, 4.0, 0.0, 0.0),
                MakeSample(502, 2, 8.0, 0.0, 0.0),
                MakeSample(503, 3, 3.999, 0.0, 0.0),
                MakeSample(504, 4, 8.001, 0.0, 0.0),
                MakeSample(505, 5, 6.0, 2.25, 0.0),
                MakeSample(506, 6, 6.0, 2.250001, 0.0)
            };
            batch.Rebuild(samples);
            std::vector<SurvivorRadialTargetHit> hits;
            batch.QuerySweptAnnulus(0.0, 0.0, 0.0, 5.0, 7.0, 0.5, 2.25, hits);
            RequireKeys(hits, { { 501, 1 }, { 502, 2 }, { 505, 5 } },
                "Swept annulus changed inner/outer/vertical inclusive boundaries or stable order.");

            const std::vector<SurvivorTargetSample> centerSample = {
                MakeSample(507, 7, 0.0, 0.0, 0.0)
            };
            batch.Rebuild(centerSample);
            batch.QuerySweptAnnulus(0.0, 0.0, 0.0, 0.2, 0.2, 0.3, 0.0, hits);
            RequireKeys(hits, { { 507, 7 } },
                "Swept annulus failed to clamp a negative inner edge to zero.");
        }

        void CircleOverlapContract()
        {
            SurvivorTargetingBatch batch(4.0);
            const std::vector<SurvivorTargetSample> samples = {
                MakeSample(601, 5, 0.5, 0.0, 0.0),
                MakeSample(602, 2, -0.5, 0.0, 0.0),
                MakeSample(603, 6, 1.5, 0.0, 0.0),
                MakeSample(604, 7, 1.501, 0.0, 0.0),
                MakeSample(605, 8, 3.0, 0.0, 0.0, 2.0),
                MakeSample(606, 9, 0.0, 2.25, 1.0),
                MakeSample(607, 10, 0.0, 2.250001, 1.0)
            };
            batch.Rebuild(samples);
            std::vector<SurvivorRadialTargetHit> hits;
            batch.QueryCircleOverlaps(0.0, 0.0, 0.0, 1.0, 2.25, hits);
            RequireKeys(hits,
                { { 602, 2 }, { 601, 5 }, { 606, 9 }, { 603, 6 }, { 605, 8 } },
                "Circle overlap changed nearest/key order, radius expansion, or inclusive boundaries.");
            RequireNear(hits.front().DistanceSquared, 0.25,
                "Circle overlap returned the wrong nearest distance.");
            RequireNear(hits.back().DistanceSquared, 9.0,
                "Circle overlap rejected a large-radius target at exact contact.");
        }

        void CapsuleContract()
        {
            SurvivorTargetingBatch batch(4.0);
            const std::vector<SurvivorTargetSample> samples = {
                MakeSample(701, 2, -1.0, 100.0, 0.0),
                MakeSample(702, 5, 11.0, -100.0, 0.0),
                MakeSample(703, 4, 5.0, 0.0, 1.0),
                MakeSample(704, 1, 5.0, 0.0, -1.0),
                MakeSample(705, 6, 5.0, 0.0, 1.001),
                MakeSample(706, 7, -1.001, 0.0, 0.0),
                MakeSample(707, 8, 11.001, 0.0, 0.0)
            };
            batch.Rebuild(samples);
            std::vector<SurvivorCapsuleTargetHit> hits;
            batch.QuerySweptCapsule(0.0, 0.0, 10.0, 0.0, 0.5, hits);
            RequireKeys(hits, { { 701, 2 }, { 704, 1 }, { 703, 4 }, { 702, 5 } },
                "Swept capsule changed endpoint caps, tangent contacts, planar behavior, or t/key order.");
            RequireNear(hits[0].SegmentFraction, 0.0,
                "Swept capsule returned the wrong start-cap fraction.");
            RequireNear(hits[1].SegmentFraction, 0.5,
                "Swept capsule returned the wrong body fraction.");
            RequireNear(hits.back().SegmentFraction, 1.0,
                "Swept capsule returned the wrong end-cap fraction.");

            hits.push_back({ MakeSample(799, 99, 0.0, 0.0, 0.0), 0.0 });
            batch.QuerySweptCapsule(0.0, 0.0, 0.0009, 0.0, 0.5, hits);
            Require(hits.empty(),
                "A swept capsule below the existing 1e-6 segment-length-squared threshold returned hits.");

            const std::vector<SurvivorTargetSample> generationSamples = {
                MakeSample(750, 100, 5.0, 0.0, 0.0, 0.5, false, false),
                MakeSample(750, 101, 5.0, 0.0, 0.0)
            };
            batch.Rebuild(generationSamples);
            batch.QuerySweptCapsule(0.0, 0.0, 10.0, 0.0, 0.5, hits);
            RequireKeys(hits, { { 750, 101 } },
                "Swept capsule leaked a previous pooled activation generation.");

            const std::unordered_set<SurvivorTargetKey, SurvivorTargetKeyHash> castCohort = {
                { 750, 100 }
            };
            Require(!castCohort.contains(hits.front().Target.Key),
                "A new activation incorrectly inherited an old cast cohort key.");
        }

        void CrossCellReferenceParityContract()
        {
            std::vector<SurvivorTargetSample> samples;
            samples.reserve(320);
            std::unordered_set<int64_t> occupiedXCells;
            std::unordered_set<int64_t> occupiedZCells;
            for (uint32_t index = 0; index < 320; ++index)
            {
                const int column = static_cast<int>(index % 20);
                const int row = static_cast<int>(index / 20);
                const double x =
                    -83.75 + column * 4.35 + (static_cast<int>(index % 3) - 1) * 0.17;
                const double y = (static_cast<int>(index % 9) - 4) * 0.45;
                const double z =
                    -67.25 + row * 4.60 + (static_cast<int>(index % 5) - 2) * 0.11;
                const double radius = 0.22 + (index % 6) * 0.09;
                const bool bActive = index % 37 != 0;
                const bool bDead = index % 41 == 0;
                samples.push_back(MakeSample(
                    20000 + index,
                    30000 + (index * 73) % 320,
                    x,
                    y,
                    z,
                    radius,
                    bActive,
                    bDead));
                occupiedXCells.insert(static_cast<int64_t>(std::floor(x / 4.0)));
                occupiedZCells.insert(static_cast<int64_t>(std::floor(z / 4.0)));
            }
            Require(samples.size() == 320 &&
                occupiedXCells.size() > 20 &&
                occupiedZCells.size() > 15,
                "The parity corpus no longer spans 320 samples across negative grid cells.");
            Require(std::ranges::all_of(samples, [](const SurvivorTargetSample& sample)
            {
                return sample.X < 0.0;
            }), "The parity corpus lost its negative-X coverage.");

            SurvivorTargetingBatch batch(4.0);
            batch.Rebuild(samples);

            const std::vector<SurvivorCorridorQuery> corridors = {
                { -84.0, -67.0, 1.0, 0.0, 0.12, 92.0, 3.0, 24 },
                { -84.0, -67.0, 0.8, 0.6, 0.12, 118.0, 2.5, 20 },
                { -42.0, -68.0, 0.0, 1.0, 0.12, 75.0, 2.8, 16 }
            };
            std::vector<std::vector<SurvivorCorridorTargetHit>> corridorHits;
            batch.QueryCorridors(
                -42.0,
                0.0,
                -32.0,
                115.0,
                2.0,
                corridors,
                corridorHits);
            const std::vector<std::vector<SurvivorCorridorTargetHit>> corridorReference =
                ReferenceCorridors(
                    samples,
                    -42.0,
                    0.0,
                    -32.0,
                    115.0,
                    2.0,
                    corridors);
            Require(std::ranges::any_of(corridorReference, [](const auto& hits)
            {
                return !hits.empty();
            }), "The 320-sample corridor reference corpus did not exercise a hit.");
            RequireCorridorsMatchReference(
                corridorHits,
                corridorReference,
                "The 320-sample cross-cell corridor batch disagreed with brute force.");

            std::vector<SurvivorRadialTargetHit> radialHits;
            batch.SelectChain(-83.0, 0.0, -66.0, 115.0, 2.0, 6.6, 15, radialHits);
            const std::vector<SurvivorRadialTargetHit> chainReference = ReferenceChain(
                samples,
                -83.0,
                0.0,
                -66.0,
                115.0,
                2.0,
                6.6,
                15);
            Require(chainReference.size() > 1,
                "The 320-sample chain reference corpus did not exercise a link.");
            RequireRadialMatchesReference(
                radialHits,
                chainReference,
                "The 320-sample cross-cell chain disagreed with brute force.");

            batch.QuerySweptAnnulus(
                -50.0,
                0.0,
                -30.0,
                15.0,
                40.0,
                1.0,
                2.0,
                radialHits);
            const std::vector<SurvivorRadialTargetHit> annulusReference =
                ReferenceSweptAnnulus(
                    samples,
                    -50.0,
                    0.0,
                    -30.0,
                    15.0,
                    40.0,
                    1.0,
                    2.0);
            Require(!annulusReference.empty(),
                "The 320-sample annulus reference corpus did not exercise a hit.");
            RequireRadialMatchesReference(
                radialHits,
                annulusReference,
                "The 320-sample cross-cell annulus disagreed with brute force.");

            batch.QueryCircleOverlaps(-40.0, 0.0, -35.0, 3.5, 2.0, radialHits);
            const std::vector<SurvivorRadialTargetHit> circleReference =
                ReferenceCircleOverlaps(
                    samples,
                    -40.0,
                    0.0,
                    -35.0,
                    3.5,
                    2.0);
            Require(!circleReference.empty(),
                "The 320-sample circle reference corpus did not exercise a hit.");
            RequireRadialMatchesReference(
                radialHits,
                circleReference,
                "The 320-sample cross-cell circle overlap disagreed with brute force.");

            std::vector<SurvivorCapsuleTargetHit> capsuleHits;
            batch.QuerySweptCapsule(-85.0, -68.0, -5.0, 0.0, 3.0, capsuleHits);
            const std::vector<SurvivorCapsuleTargetHit> capsuleReference =
                ReferenceSweptCapsule(samples, -85.0, -68.0, -5.0, 0.0, 3.0);
            Require(!capsuleReference.empty(),
                "The 320-sample capsule reference corpus did not exercise a hit.");
            RequireCapsuleMatchesReference(
                capsuleHits,
                capsuleReference,
                "The 320-sample cross-cell capsule disagreed with brute force.");
        }

        void RebuildAndExtremeGridContract()
        {
            SurvivorTargetingBatch batch(4.0);
            std::vector<SurvivorTargetSample> largeSamples;
            largeSamples.reserve(128);
            for (uint32_t index = 0; index < 128; ++index)
            {
                largeSamples.push_back(MakeSample(
                    40000 + index,
                    50000 + index,
                    -70.25 + (index % 16) * 4.5,
                    0.0,
                    -55.75 + (index / 16) * 5.25,
                    0.3 + (index % 4) * 0.1));
            }
            batch.Rebuild(largeSamples);
            std::vector<SurvivorRadialTargetHit> hits;
            batch.QueryRadius(
                largeSamples[1].X,
                largeSamples[1].Y,
                largeSamples[1].Z,
                0.0,
                0.0,
                hits);
            RequireKeys(hits, { largeSamples[1].Key },
                "The large pre-rebuild snapshot was not queryable at an exact cell location.");

            const std::vector<SurvivorTargetSample> smallSamples = {
                MakeSample(60001, 70001, -120.125, 0.0, -96.875, 0.25),
                MakeSample(60002, 70002, 124.375, 0.0, 108.625, 0.75)
            };
            batch.Rebuild(smallSamples);
            Require(batch.GetSampleCount() == 2 &&
                batch.GetMaximumTargetRadius() == 0.75,
                "A large-to-small rebuild retained stale samples or maximum radius.");
            batch.QueryRadius(
                largeSamples[1].X,
                largeSamples[1].Y,
                largeSamples[1].Z,
                0.0,
                0.0,
                hits);
            Require(hits.empty(),
                "A large-to-small rebuild leaked an entry from an old grid cell.");
            batch.QueryRadius(-120.125, 0.0, -96.875, 0.0, 0.0, hits);
            RequireKeys(hits, { { 60001, 70001 } },
                "A cross-grid rebuild lost the new negative-cell sample.");
            batch.QueryRadius(124.375, 0.0, 108.625, 0.0, 0.0, hits);
            RequireKeys(hits, { { 60002, 70002 } },
                "A cross-grid rebuild lost the new positive-cell sample.");

            const double maximumCoordinate = std::numeric_limits<double>::max();
            const std::vector<SurvivorTargetSample> saturatedSamples = {
                MakeSample(61001, 71001, maximumCoordinate, 0.0, maximumCoordinate)
            };
            batch.Rebuild(saturatedSamples);
            batch.QueryRadius(
                maximumCoordinate,
                0.0,
                maximumCoordinate,
                0.0,
                0.0,
                hits);
            RequireKeys(hits, { { 61001, 71001 } },
                "A saturated INT64_MAX grid cell did not terminate or return its exact sample.");

            const double halfMaximumCoordinate = maximumCoordinate * 0.5;
            const std::vector<SurvivorTargetSample> hugeSpanSamples = {
                MakeSample(62001, 72001, halfMaximumCoordinate, 0.0, 0.0),
                MakeSample(
                    62002,
                    72002,
                    halfMaximumCoordinate,
                    0.0,
                    halfMaximumCoordinate)
            };
            batch.Rebuild(hugeSpanSamples);
            const SurvivorTargetingQueryStats hugeSpanStats = batch.QueryRadius(
                0.0,
                0.0,
                0.0,
                halfMaximumCoordinate,
                0.0,
                hits);
            RequireKeys(hits, { { 62001, 72001 } },
                "The huge-span fallback changed overflow-safe radius membership.");
            Require(hugeSpanStats.VisitedCellCount == 0 &&
                hugeSpanStats.ExaminedSampleCount == hugeSpanSamples.size(),
                "A huge grid span did not use the bounded full-scan fallback.");
        }
    }

    void SurvivorTargetingBatchContract()
    {
        std::vector<SurvivorTargetSample> denseSamples;
        denseSamples.reserve(40000);
        for (uint32_t i = 0; i < 40000; ++i)
            denseSamples.push_back(MakeSample(i + 1, i + 1, double(i), 0.0, 0.0,
                0.5, i % 10 != 0));
        SurvivorTargetingBatch denseBatch;
        denseBatch.Rebuild(denseSamples);
        Require(denseBatch.GetSampleCount() == 36000,
            "Parallel snapshot filtering changed the retained count.");
        std::vector<SurvivorRadialTargetHit> denseHits;
        (void)denseBatch.QueryRadius(0.0, 0.0, 0.0, 50000.0, 1.0, denseHits);
        Require(denseHits.size() == 36000 &&
            denseHits.front().Target.Key.EntityId == 2 &&
            denseHits.back().Target.Key.EntityId == 40000,
            "Parallel snapshot preparation changed stable query output.");
        RadiusAndGenerationContract();
        DensestContract();
        CorridorContract();
        ChainContract();
        AnnulusContract();
        CircleOverlapContract();
        CapsuleContract();
        CrossCellReferenceParityContract();
        RebuildAndExtremeGridContract();
    }

    void SurvivorTargetingBatchBenchmark()
    {
        for (const uint32_t sampleCount : {9000u, 100000u})
        {
            std::vector<SurvivorTargetSample> samples;
            samples.reserve(sampleCount);
            for (uint32_t i = 0; i < sampleCount; ++i)
                samples.push_back(MakeSample(i + 1, i + 1,
                    double(i % 1000) * 0.5, 0.0, double(i / 1000) * 0.5));
            SurvivorTargetingBatch batch;
            std::vector<double> milliseconds;
            for (int repeat = 0; repeat < 20; ++repeat)
            {
                const auto begin = std::chrono::steady_clock::now();
                batch.Rebuild(samples);
                milliseconds.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - begin).count());
            }
            std::sort(milliseconds.begin(), milliseconds.end());
            HE_CORE_INFO("TargetingRebuildBenchmark samples={} repeats=20 p50Ms={:.3f} p95Ms={:.3f}",
                sampleCount, milliseconds[10], milliseconds[19]);
            Require(batch.GetSampleCount() == samples.size(),
                "Benchmark snapshot rebuild lost samples.");
        }
    }
}
