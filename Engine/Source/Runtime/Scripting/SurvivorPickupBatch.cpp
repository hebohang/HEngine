#include "Hepch.h"

#include "Runtime/Scripting/SurvivorPickupBatch.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace HEngine
{
    namespace
    {
        double SurvivorPickupSinApprox(double value)
        {
            constexpr double pi = 3.141592653589793;
            constexpr double twoPi = pi * 2.0;
            value = std::fmod(value + pi, twoPi);
            if (value < 0.0)
                value += twoPi;
            value -= pi;
            const double valueSquared = value * value;
            return value * (
                1.0 - valueSquared / 6.0 +
                valueSquared * valueSquared / 120.0);
        }
    }

    void SurvivorPickupBatch::Clear()
    {
        mStates.clear();
        mStateIndices.clear();
        mLatestActivationIds.clear();
    }

    bool SurvivorPickupBatch::Activate(SurvivorPickupState state)
    {
        const auto latestIterator = mLatestActivationIds.find(state.EntityId);
        if (latestIterator != mLatestActivationIds.end() &&
            state.ActivationId <= latestIterator->second)
        {
            return false;
        }

        const auto stateIterator = mStateIndices.find(state.EntityId);
        if (stateIterator != mStateIndices.end())
        {
            mStates.erase(mStates.begin() + static_cast<std::ptrdiff_t>(stateIterator->second));
            RebuildStateIndices();
        }

        state.bActive = true;
        mLatestActivationIds[state.EntityId] = state.ActivationId;
        mStateIndices[state.EntityId] = mStates.size();
        mStates.push_back(std::move(state));
        return true;
    }

    bool SurvivorPickupBatch::Deactivate(uint32_t entityId, uint64_t activationId)
    {
        SurvivorPickupState* state = FindActiveState(entityId, activationId);
        if (!state)
            return false;
        state->bActive = false;
        return true;
    }

    bool SurvivorPickupBatch::SetPosition(
        uint32_t entityId,
        uint64_t activationId,
        double x,
        double y,
        double z)
    {
        SurvivorPickupState* state = FindActiveState(entityId, activationId);
        if (!state)
            return false;
        state->X = x;
        state->Y = y;
        state->Z = z;
        return true;
    }

    bool SurvivorPickupBatch::SetScaleBonus(
        uint32_t entityId,
        uint64_t activationId,
        double scaleBonus)
    {
        SurvivorPickupState* state = FindActiveState(entityId, activationId);
        if (!state)
            return false;
        state->ScaleBonus = scaleBonus;
        return true;
    }

    void SurvivorPickupBatch::Update(
        const SurvivorPickupBatchInput& input,
        std::vector<SurvivorPickupIdentity>& outCollected)
    {
        outCollected.clear();
        if (outCollected.capacity() < mStates.size())
            outCollected.reserve(mStates.size());

        const double magnetRangeSquared = input.MagnetRange * input.MagnetRange;
        const double pickupRangeSquared = input.PickupRange * input.PickupRange;
        for (SurvivorPickupState& state : mStates)
        {
            if (!state.bActive)
                continue;

            state.Age += input.DeltaTime;
            state.Rotation += input.DeltaTime * 2.8;

            const double deltaX = input.PlayerX - state.X;
            const double deltaZ = input.PlayerZ - state.Z;
            // Magnet and collection membership intentionally use the same
            // pre-move horizontal distance as the Python gameplay contract.
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
                state.Y = state.BaseY + SurvivorPickupSinApprox(
                    state.Age * 3.4 + state.Phase) * 0.08;
            }

            if (distanceSquared <= pickupRangeSquared)
            {
                state.bActive = false;
                outCollected.push_back({ state.EntityId, state.ActivationId });
            }
        }
    }

    void SurvivorPickupBatch::PurgeInactive()
    {
        std::erase_if(mStates, [](const SurvivorPickupState& state)
        {
            return !state.bActive;
        });
        RebuildStateIndices();
    }

    std::span<const SurvivorPickupState> SurvivorPickupBatch::GetStates() const
    {
        return { mStates.data(), mStates.size() };
    }

    SurvivorPickupState* SurvivorPickupBatch::FindActiveState(
        uint32_t entityId,
        uint64_t activationId)
    {
        const auto iterator = mStateIndices.find(entityId);
        if (iterator == mStateIndices.end())
            return nullptr;

        SurvivorPickupState& state = mStates[iterator->second];
        if (!state.bActive || state.ActivationId != activationId)
            return nullptr;
        return &state;
    }

    void SurvivorPickupBatch::RebuildStateIndices()
    {
        mStateIndices.clear();
        mStateIndices.reserve(mStates.size());
        for (size_t index = 0; index < mStates.size(); ++index)
            mStateIndices[mStates[index].EntityId] = index;
    }
}
