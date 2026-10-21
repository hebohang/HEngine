#pragma once

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    // One lease per recording, not per view ID: a view can record more than once.
    // The host must publish every issued submission, even if presentation fails.
    class EffekseerRecordLeases
    {
    public:
        struct Lease { uint32_t FrameSlot; size_t Record; };
        explicit EffekseerRecordLeases(uint32_t count = 0) : mBanks(count) {}
        void Begin(uint32_t slot, uint64_t completed)
        {
            if (mActive || slot >= mBanks.size())
                throw std::logic_error("Effekseer invalid or overlapping host frame");
            auto& bank = mBanks[slot];
            if (bank.LastSubmission > completed)
                throw std::logic_error("Effekseer host frame resources are still in flight");
            bank.Cursor = 0;
            mSlot = slot;
            mActive = true;
        }
        [[nodiscard]] Lease Acquire()
        {
            if (!mActive)
                throw std::logic_error("Effekseer recording requires a host frame lease");
            return {mSlot, mBanks[mSlot].Cursor++};
        }
        void Submitted(uint32_t slot, uint64_t submission) noexcept
        {
            if (!mActive || slot != mSlot || submission == 0)
                return; // Duplicate publication must not affect a later bank.
            auto& bank = mBanks[slot];
            if (submission <= bank.LastSubmission)
                return; // A late duplicate must not close the new recording scope.
            bank.LastSubmission = submission;
            mActive = false;
        }
        void Abort(uint32_t slot) noexcept
        {
            if (mActive && slot == mSlot)
                mActive = false; // Never clear the previous submission fence.
        }
    private:
        struct Bank { uint64_t LastSubmission = 0; size_t Cursor = 0; };
        std::vector<Bank> mBanks;
        uint32_t mSlot = 0;
        bool mActive = false;
    };
}
