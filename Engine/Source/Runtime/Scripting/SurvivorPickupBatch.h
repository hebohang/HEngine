#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    struct SurvivorPickupIdentity
    {
        uint32_t EntityId = 0;
        uint64_t ActivationId = 0;

        bool operator==(const SurvivorPickupIdentity&) const = default;
    };

    struct SurvivorPickupState
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

    struct SurvivorPickupBatchInput
    {
        double PlayerX = 0.0;
        double PlayerY = 0.0;
        double PlayerZ = 0.0;
        double DeltaTime = 0.0;
        double MagnetRange = 0.0;
        double PickupRange = 0.0;
    };

    class SurvivorPickupBatch
    {
    public:
        void Clear();

        // A newer activation of a pooled entity replaces the previous generation
        // and is appended at the end, preserving gameplay activation order.
        // Duplicate or stale generations are rejected without mutating live state.
        [[nodiscard]] bool Activate(SurvivorPickupState state);
        [[nodiscard]] bool Deactivate(uint32_t entityId, uint64_t activationId);
        [[nodiscard]] bool SetPosition(
            uint32_t entityId,
            uint64_t activationId,
            double x,
            double y,
            double z);
        [[nodiscard]] bool SetScaleBonus(
            uint32_t entityId,
            uint64_t activationId,
            double scaleBonus);

        // DeltaTime is one aggregate gameplay pickup step. In particular, 2/30
        // is intentionally evaluated once rather than as two 1/30 substeps.
        void Update(
            const SurvivorPickupBatchInput& input,
            std::vector<SurvivorPickupIdentity>& outCollected);

        // Update leaves just-collected states available for the ECS adapter to
        // apply their final transform before Python performs pooling callbacks.
        void PurgeInactive();

        [[nodiscard]] std::span<const SurvivorPickupState> GetStates() const;
        [[nodiscard]] size_t GetStateCount() const { return mStates.size(); }

    private:
        [[nodiscard]] SurvivorPickupState* FindActiveState(
            uint32_t entityId,
            uint64_t activationId);
        void RebuildStateIndices();

        std::vector<SurvivorPickupState> mStates;
        std::unordered_map<uint32_t, size_t> mStateIndices;
        // Tombstones remain until Clear so a delayed pooled-entity callback
        // cannot resurrect or mutate an already superseded generation.
        std::unordered_map<uint32_t, uint64_t> mLatestActivationIds;
    };
}
