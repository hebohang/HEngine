#include "Hepch.h"

#include "Runtime/UnitTest/SurvivorPickupBatchTests.h"
#include "Runtime/Scripting/SurvivorPickupBatch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
        struct ReferencePickupState
        {
            uint32_t EntityId = 0;
            uint64_t ActivationId = 0;
            double X = 0.0;
            double Y = 0.0;
            double Z = 0.0;
            double BaseY = 0.0;
            double Age = 0.0;
            double Phase = 0.0;
            double Rotation = 0.0;
            double ScaleBonus = 0.0;
            bool bMagnetized = false;
            bool bActive = true;
        };

        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        void RequireNear(double actual, double expected, std::string_view message)
        {
            constexpr double tolerance = 1.0e-12;
            if (std::abs(actual - expected) > tolerance)
                throw std::runtime_error(std::string(message));
        }

        double ReferenceSinApprox(double value)
        {
            constexpr double pi = 3.141592653589793;
            value = std::fmod(value + pi, 2.0 * pi);
            if (value < 0.0)
                value += 2.0 * pi;
            value -= pi;
            const double valueSquared = value * value;
            return value * (
                1.0 - valueSquared / 6.0 +
                valueSquared * valueSquared / 120.0);
        }

        std::vector<SurvivorPickupIdentity> ReferenceUpdate(
            std::span<ReferencePickupState> states,
            const SurvivorPickupBatchInput& input)
        {
            std::vector<SurvivorPickupIdentity> collected;
            const double magnetRangeSquared = input.MagnetRange * input.MagnetRange;
            const double pickupRangeSquared = input.PickupRange * input.PickupRange;
            for (ReferencePickupState& state : states)
            {
                if (!state.bActive)
                    continue;

                state.Age += input.DeltaTime;
                state.Rotation += input.DeltaTime * 2.8;
                const double deltaX = input.PlayerX - state.X;
                const double deltaZ = input.PlayerZ - state.Z;
                const double distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
                if (distanceSquared <= magnetRangeSquared)
                    state.bMagnetized = true;

                if (state.bMagnetized)
                {
                    const double distance = std::max(0.001, std::sqrt(distanceSquared));
                    const double speed = std::min(20.0, 5.0 + state.Age * 7.0);
                    const double move = std::min(distance, speed * input.DeltaTime);
                    state.X += deltaX / distance * move;
                    state.Z += deltaZ / distance * move;
                    state.Y += ((input.PlayerY + 0.25) - state.Y) *
                        std::min(1.0, input.DeltaTime * 8.0);
                }
                else
                {
                    state.Y = state.BaseY + ReferenceSinApprox(
                        state.Age * 3.4 + state.Phase) * 0.08;
                }

                if (distanceSquared <= pickupRangeSquared)
                {
                    state.bActive = false;
                    collected.push_back({ state.EntityId, state.ActivationId });
                }
            }
            return collected;
        }

        SurvivorPickupState ToProductionState(const ReferencePickupState& state)
        {
            return {
                state.EntityId,
                state.ActivationId,
                state.X,
                state.Y,
                state.Z,
                state.BaseY,
                state.Age,
                state.Phase,
                state.Rotation,
                state.ScaleBonus,
                state.bMagnetized,
                state.bActive
            };
        }

        ReferencePickupState MakeReferenceState(
            uint32_t entityId,
            uint64_t activationId,
            double x,
            double y,
            double z,
            double baseY = 0.04,
            double age = 0.0,
            double phase = 0.0,
            double rotation = 0.0,
            double scaleBonus = 0.0,
            bool bMagnetized = false)
        {
            return {
                entityId,
                activationId,
                x,
                y,
                z,
                baseY,
                age,
                phase,
                rotation,
                scaleBonus,
                bMagnetized,
                true
            };
        }

        void CompareState(
            const SurvivorPickupState& actual,
            const ReferencePickupState& expected)
        {
            Require(actual.EntityId == expected.EntityId &&
                actual.ActivationId == expected.ActivationId,
                "The pickup batch changed identity or activation order.");
            RequireNear(actual.X, expected.X, "The pickup batch changed X movement.");
            RequireNear(actual.Y, expected.Y, "The pickup batch changed Y movement.");
            RequireNear(actual.Z, expected.Z, "The pickup batch changed Z movement.");
            RequireNear(actual.BaseY, expected.BaseY, "The pickup batch changed base Y.");
            RequireNear(actual.Age, expected.Age, "The pickup batch changed age accumulation.");
            RequireNear(actual.Phase, expected.Phase, "The pickup batch changed bob phase.");
            RequireNear(actual.Rotation, expected.Rotation,
                "The pickup batch changed rotation accumulation.");
            RequireNear(actual.ScaleBonus, expected.ScaleBonus,
                "The pickup batch changed the scale bonus.");
            Require(actual.bMagnetized == expected.bMagnetized,
                "The pickup batch changed sticky magnet state.");
            Require(actual.bActive == expected.bActive,
                "The pickup batch changed collection state.");
        }

        std::vector<ReferencePickupState> MakeMatrixFixture()
        {
            std::vector<ReferencePickupState> states;
            states.reserve(220);
            for (uint32_t index = 0; index < 220; ++index)
            {
                const double x = (static_cast<int>(index % 20) - 10) * 0.47 +
                    static_cast<double>(index % 3) * 0.013;
                const double z = (static_cast<int>(index / 20) - 5) * 0.53 -
                    static_cast<double>(index % 5) * 0.017;
                const double baseY = 0.04 + static_cast<double>(index % 4) * 0.011;
                const double y = baseY + static_cast<double>(index % 7) * 0.037;
                states.push_back(MakeReferenceState(
                    10000 + ((index * 73) % 220),
                    9007199254740993ULL + index,
                    x,
                    y,
                    z,
                    baseY,
                    static_cast<double>(index % 17) * 0.19,
                    static_cast<double>(index % 23) * 0.271 - 2.7,
                    static_cast<double>(index % 29) * 0.31,
                    static_cast<double>(index % 9) * 0.012,
                    index % 13 == 0));
            }
            return states;
        }

        void RunReferenceMatrix(double deltaTime)
        {
            const SurvivorPickupBatchInput input = {
                0.4,
                1.1,
                -0.6,
                deltaTime,
                4.2,
                0.85
            };
            std::vector<ReferencePickupState> expectedStates = MakeMatrixFixture();
            SurvivorPickupBatch batch;
            for (const ReferencePickupState& state : expectedStates)
            {
                Require(batch.Activate(ToProductionState(state)),
                    "A unique matrix activation was rejected.");
            }

            const std::vector<SurvivorPickupIdentity> expectedCollected =
                ReferenceUpdate(expectedStates, input);
            std::vector<SurvivorPickupIdentity> actualCollected = {
                { std::numeric_limits<uint32_t>::max(),
                    std::numeric_limits<uint64_t>::max() }
            };
            batch.Update(input, actualCollected);

            Require(batch.GetStateCount() == 220,
                "The pickup batch did not retain all states until PurgeInactive.");
            Require(actualCollected == expectedCollected,
                "The 220-pickup collection membership or activation order changed.");
            std::unordered_set<uint32_t> uniqueCollectedEntities;
            for (const SurvivorPickupIdentity& identity : actualCollected)
            {
                Require(uniqueCollectedEntities.insert(identity.EntityId).second,
                    "The 220-pickup batch returned a duplicate collection identity.");
            }

            const std::span<const SurvivorPickupState> actualStates = batch.GetStates();
            Require(actualStates.size() == expectedStates.size(),
                "The 220-pickup batch changed state count before purge.");
            for (size_t index = 0; index < actualStates.size(); ++index)
                CompareState(actualStates[index], expectedStates[index]);
        }

        void AggregateDeltaTimeContract()
        {
            const SurvivorPickupBatchInput aggregateInput = {
                0.0, 1.0, 0.0, 2.0 / 30.0, 4.2, 0.85
            };
            const ReferencePickupState initial = MakeReferenceState(
                401, 1, 3.5, 0.3, 0.0, 0.04, 0.8, 0.4, 0.2);

            SurvivorPickupBatch batch;
            Require(batch.Activate(ToProductionState(initial)),
                "The aggregate-dt sentinel was rejected.");
            std::vector<SurvivorPickupIdentity> collected;
            batch.Update(aggregateInput, collected);

            std::vector<ReferencePickupState> aggregateReference = { initial };
            ReferenceUpdate(aggregateReference, aggregateInput);
            CompareState(batch.GetStates().front(), aggregateReference.front());

            std::vector<ReferencePickupState> substepReference = { initial };
            SurvivorPickupBatchInput substepInput = aggregateInput;
            substepInput.DeltaTime = 1.0 / 30.0;
            ReferenceUpdate(substepReference, substepInput);
            ReferenceUpdate(substepReference, substepInput);
            Require(std::abs(
                aggregateReference.front().X - substepReference.front().X) > 1.0e-6,
                "The aggregate-dt sentinel cannot distinguish one 2/30 step from two substeps.");
        }

        void BoundaryAndStickyContract()
        {
            SurvivorPickupBatch batch;
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                501, 1, 4.2, 0.04, 0.0))),
                "The magnet-boundary pickup was rejected.");
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                502, 2, std::nextafter(4.2, std::numeric_limits<double>::infinity()),
                0.04, 0.0))),
                "The outside-magnet pickup was rejected.");
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                503, 3, 0.85, 100.0, 0.0))),
                "The pickup-boundary entry was rejected.");
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                504, 4, std::nextafter(0.85, std::numeric_limits<double>::infinity()),
                0.04, 0.0, 0.04, 10.0))),
                "The outside-pickup entry was rejected.");
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                505, 5, 10.0, 8.0, 0.0, 2.0, 0.2, -0.7))),
                "The bobbing pickup was rejected.");

            const SurvivorPickupBatchInput input = {
                0.0, 1.0, 0.0, 1.0 / 30.0, 4.2, 0.85
            };
            std::vector<SurvivorPickupIdentity> collected;
            batch.Update(input, collected);
            const std::span<const SurvivorPickupState> states = batch.GetStates();
            Require(states[0].bMagnetized && states[0].X < 4.2,
                "The inclusive magnet boundary did not become sticky.");
            Require(!states[1].bMagnetized,
                "A pickup outside the magnet boundary became sticky.");
            Require(!states[2].bActive,
                "The inclusive XZ pickup boundary ignored a large Y separation.");
            Require(states[3].bActive && states[3].X < 0.85,
                "Collection used the post-move position instead of the pre-move distance.");
            RequireNear(
                states[4].Y,
                2.0 + ReferenceSinApprox((0.2 + 1.0 / 30.0) * 3.4 - 0.7) * 0.08,
                "A non-magnetized pickup did not overwrite Y with its bob curve.");
            Require(collected.size() == 1 && collected.front() == SurvivorPickupIdentity{ 503, 3 },
                "Boundary collection returned the wrong identity or a post-move pickup.");

            batch.Update(input, collected);
            Require(!batch.GetStates()[3].bActive,
                "A pickup that crossed the collection radius was not collected on the next step.");

            const double stickyX = batch.GetStates()[0].X;
            SurvivorPickupBatchInput movedPlayerInput = input;
            movedPlayerInput.PlayerX = -20.0;
            movedPlayerInput.MagnetRange = 0.1;
            batch.Update(movedPlayerInput, collected);
            Require(batch.GetStates()[0].bMagnetized && batch.GetStates()[0].X < stickyX,
                "A magnetized pickup stopped following after the player left magnet range.");
        }

        void ZeroDistanceContract()
        {
            SurvivorPickupBatch batch;
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                601, 1, 0.0, 0.25, 0.0))),
                "The zero-distance pickup was rejected.");
            std::vector<SurvivorPickupIdentity> collected;
            batch.Update({ 0.0, 0.0, 0.0, 1.0 / 30.0, 4.2, 0.85 }, collected);
            const SurvivorPickupState& state = batch.GetStates().front();
            Require(std::isfinite(state.X) && std::isfinite(state.Y) &&
                std::isfinite(state.Z),
                "Zero-distance normalization produced a non-finite transform.");
            Require(collected.size() == 1,
                "A zero-distance pickup was not collected exactly once.");
            batch.Update({ 0.0, 0.0, 0.0, 1.0 / 30.0, 4.2, 0.85 }, collected);
            Require(collected.empty(),
                "An inactive pickup was returned by a second update.");
        }

        void GenerationAndPurgeContract()
        {
            SurvivorPickupBatch batch;
            SurvivorPickupState first = ToProductionState(MakeReferenceState(
                701, 10, 0.5, 0.04, 0.0));
            Require(batch.Activate(first), "The first pooled generation was rejected.");
            SurvivorPickupState duplicate = first;
            duplicate.X = 99.0;
            duplicate.Age = 7.0;
            duplicate.ScaleBonus = 0.16;
            Require(!batch.Activate(duplicate) && batch.GetStateCount() == 1,
                "A duplicate activation created or reset a pickup state.");
            RequireNear(batch.GetStates().front().X, first.X,
                "A duplicate activation changed live position.");
            RequireNear(batch.GetStates().front().Age, first.Age,
                "A duplicate activation changed live age.");
            RequireNear(batch.GetStates().front().ScaleBonus, first.ScaleBonus,
                "A duplicate activation changed live scale bonus.");

            std::vector<SurvivorPickupIdentity> collected;
            batch.Update({ 0.0, 0.0, 0.0, 1.0 / 30.0, 4.2, 0.85 }, collected);
            Require(collected.size() == 1 &&
                collected.front() == SurvivorPickupIdentity{ 701, 10 },
                "The first pooled generation was not collected exactly once.");
            batch.Update({ 0.0, 0.0, 0.0, 1.0 / 30.0, 4.2, 0.85 }, collected);
            Require(collected.empty(),
                "A collected generation was returned twice before purge.");
            batch.PurgeInactive();
            Require(batch.GetStateCount() == 0,
                "PurgeInactive retained a collected pickup.");
            Require(!batch.Activate(ToProductionState(MakeReferenceState(
                701, 9, 5.0, 0.04, 0.0))) &&
                !batch.Activate(first),
                "A stale or equal activation resurrected a purged generation.");

            SurvivorPickupState second = ToProductionState(MakeReferenceState(
                701, 11, 6.0, 0.04, 0.0));
            Require(batch.Activate(second),
                "A newer pooled generation was rejected.");
            Require(!batch.Deactivate(701, 10),
                "A stale deactivate removed a newer pooled generation.");
            Require(!batch.SetScaleBonus(701, 10, 0.16),
                "A stale scale update mutated a newer pooled generation.");
            Require(batch.SetScaleBonus(701, 11, 0.144),
                "The live generation rejected a scale update.");
            Require(batch.SetPosition(701, 11, 7.0, 0.5, -2.0),
                "The live generation rejected a Transform synchronization.");
            RequireNear(batch.GetStates().front().ScaleBonus, 0.144,
                "The live generation stored the wrong scale bonus.");
            RequireNear(batch.GetStates().front().X, 7.0,
                "The live generation stored the wrong synchronized X.");
            Require(batch.Deactivate(701, 11),
                "The live generation could not be deactivated.");
            Require(!batch.Deactivate(701, 11),
                "A generation was deactivated twice.");
            batch.PurgeInactive();

            batch.Clear();
            Require(batch.Activate(first),
                "Clear retained generation tombstones across a new run.");
        }

        void MixedPurgeAndReuseContract()
        {
            SurvivorPickupBatch batch;
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                801, 1, 10.0, 0.04, 0.0))),
                "The leading active pickup was rejected.");
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                802, 2, 0.2, 0.04, 0.0))),
                "The middle collectible pickup was rejected.");
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                803, 3, -10.0, 0.04, 0.0))),
                "The trailing active pickup was rejected.");

            const SurvivorPickupBatchInput input = {
                0.0, 0.0, 0.0, 1.0 / 30.0, 4.2, 0.85
            };
            std::vector<SurvivorPickupIdentity> collected;
            batch.Update(input, collected);
            Require(collected == std::vector<SurvivorPickupIdentity>{ { 802, 2 } },
                "The mixed batch collected the wrong generation.");
            batch.PurgeInactive();
            Require(batch.GetStateCount() == 2 &&
                batch.GetStates()[0].EntityId == 801 &&
                batch.GetStates()[1].EntityId == 803,
                "A partial purge changed surviving activation order.");

            Require(batch.Activate(ToProductionState(MakeReferenceState(
                802, 4, 0.3, 0.04, 0.0))),
                "The first reused pickup generation was rejected.");
            Require(batch.GetStates()[0].EntityId == 801 &&
                batch.GetStates()[1].EntityId == 803 &&
                batch.GetStates()[2].ActivationId == 4,
                "A reused pickup was not appended after surviving activations.");
            batch.Update(input, collected);
            Require(collected == std::vector<SurvivorPickupIdentity>{ { 802, 4 } },
                "The first reused generation was not collected exactly once.");

            // Replace the just-collected tombstone before PurgeInactive. The old
            // identity must disappear and the newer generation remains last.
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                802, 5, 0.4, 0.04, 0.0))),
                "A newer generation could not replace an unpurged tombstone.");
            Require(batch.GetStateCount() == 3 &&
                batch.GetStates()[0].EntityId == 801 &&
                batch.GetStates()[1].EntityId == 803 &&
                batch.GetStates()[2].ActivationId == 5,
                "Unpurged generation replacement changed activation order or duplicated state.");
            batch.Update(input, collected);
            Require(collected == std::vector<SurvivorPickupIdentity>{ { 802, 5 } },
                "An unpurged replacement leaked the old identity or duplicated collection.");
            batch.Update(input, collected);
            Require(collected.empty(),
                "A reused generation was returned more than once.");
        }

        void GoldenConstantContract()
        {
            SurvivorPickupBatch magnetBatch;
            Require(magnetBatch.Activate(ToProductionState(MakeReferenceState(
                901,
                1,
                10.0,
                2.0,
                0.0,
                0.04,
                10.0,
                0.0,
                1.0,
                0.0,
                true))),
                "The golden magnet pickup was rejected.");
            std::vector<SurvivorPickupIdentity> collected;
            magnetBatch.Update({ 0.0, 5.0, 0.0, 0.2, 0.0, 0.0 }, collected);
            const SurvivorPickupState& magnetState = magnetBatch.GetStates().front();
            RequireNear(magnetState.X, 6.0,
                "The sticky movement speed cap is not 20 units per second.");
            RequireNear(magnetState.Y, 5.25,
                "The pickup Y target offset or dt*8 interpolation clamp changed.");
            RequireNear(magnetState.Age, 10.2,
                "The golden pickup age did not advance before movement.");
            RequireNear(magnetState.Rotation, 1.56,
                "The pickup rotation rate is not 2.8 radians per second.");
            Require(collected.empty(),
                "The golden magnet pickup was unexpectedly collected.");

            SurvivorPickupBatch bobBatch;
            Require(bobBatch.Activate(ToProductionState(MakeReferenceState(
                902, 1, 20.0, 99.0, 0.0, 3.0, 0.25, -0.5, 7.0))),
                "The golden bob pickup was rejected.");
            bobBatch.Update({ 0.0, 0.0, 0.0, 1.0 / 30.0, 1.0, 0.0 }, collected);
            const SurvivorPickupState& bobState = bobBatch.GetStates().front();
            RequireNear(bobState.Y, 3.0357546706844167,
                "The pickup bob 3.4 frequency or 0.08 amplitude changed.");
            RequireNear(bobState.Rotation, 7.093333333333333,
                "The uncapped golden rotation accumulation changed.");
        }

        void ActivationOrderContract()
        {
            SurvivorPickupBatch batch;
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                830, 9007199254740993ULL, 0.2, 0.0, 0.0))),
                "The first ordered activation was rejected.");
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                810, 9007199254740994ULL, 0.3, 0.0, 0.0))),
                "The second ordered activation was rejected.");
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                820, 9007199254740995ULL, 0.4, 0.0, 0.0))),
                "The third ordered activation was rejected.");
            Require(batch.Activate(ToProductionState(MakeReferenceState(
                830, 9007199254740996ULL, 0.2, 0.0, 0.0))),
                "A newer pooled activation was rejected.");

            std::vector<SurvivorPickupIdentity> collected;
            batch.Update({ 0.0, 0.0, 0.0, 1.0 / 30.0, 4.2, 0.85 }, collected);
            const std::vector<SurvivorPickupIdentity> expectedOrder = {
                { 810, 9007199254740994ULL },
                { 820, 9007199254740995ULL },
                { 830, 9007199254740996ULL }
            };
            Require(collected == expectedOrder,
                "Entity ordering replaced stable activation/list ordering.");
        }
    }

    void SurvivorPickupBatchContract()
    {
        SurvivorPickupBatch emptyBatch;
        std::vector<SurvivorPickupIdentity> collected = { { 1, 1 } };
        emptyBatch.Update({}, collected);
        Require(collected.empty(),
            "An empty pickup batch retained stale collected output.");

        RunReferenceMatrix(1.0 / 30.0);
        RunReferenceMatrix(2.0 / 30.0);
        AggregateDeltaTimeContract();
        BoundaryAndStickyContract();
        ZeroDistanceContract();
        GenerationAndPurgeContract();
        MixedPurgeAndReuseContract();
        GoldenConstantContract();
        ActivationOrderContract();
    }
}
