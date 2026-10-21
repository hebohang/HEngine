#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace HEngine::Detail
{
    [[nodiscard]] inline double ResolveProgressiveTraceBudget(double workBudgetMs,
        double targetFramesPerSecond, double otherGpuMs) noexcept
    {
        const double frameBudgetMs = 1000.0 / targetFramesPerSecond;
        const double traceCapMs = std::min(workBudgetMs, frameBudgetMs * 0.5);
        // Other passes consume the frame budget, not the trace allowance.
        // Delayed GPU timings can exceed the frame budget; retain bounded work
        // in that case instead of starving convergence down to single rows.
        return std::clamp(frameBudgetMs - otherGpuMs,
            traceCapMs * 0.5, traceCapMs) * 0.65;
    }

    // Render-thread owned. A reservation does not change the sample sequence.
    // One submitted band at a time bounds stale GPU work even across Reset().
    class ProgressiveSchedule
    {
    public:
        struct Band
        {
            uint64_t Generation = 0;
            uint32_t Sample = 0;
            uint32_t FirstRow = 0;
            uint32_t Rows = 0;
        };

        void Reset(uint32_t height) noexcept
        {
            ++mGeneration;
            mHeight = std::max(1u, height);
            mSubmittedRows = mCompletedRows = 0;
            mReserved = {};
        }

        [[nodiscard]] Band Reserve(uint32_t width, uint32_t target,
            double budgetMs, double millisecondsPerPixel, bool enabled) noexcept
        {
            mReserved = {};
            if (!enabled || mSubmission || !width || !mHeight ||
                mSubmittedRows / mHeight >= target)
                return {};
            // Until timestamps exist, never assume a full-width 1 spp is cheap.
            const double pixels = millisecondsPerPixel > 0 && std::isfinite(millisecondsPerPixel)
                ? std::clamp(budgetMs / millisecondsPerPixel, 1.0, 262144.0)
                : 4096.0;
            const uint32_t row = static_cast<uint32_t>(mSubmittedRows % mHeight);
            mReserved = {mGeneration, static_cast<uint32_t>(mSubmittedRows / mHeight), row,
                std::min(mHeight - row, std::max(1u, static_cast<uint32_t>(pixels / width)))};
            return mReserved;
        }

        void Submit(uint64_t submission) noexcept
        {
            if (!submission || !mReserved.Rows || mSubmission)
                return;
            mFlight = mReserved;
            mSubmission = submission;
            mSubmittedRows += mReserved.Rows;
            mReserved = {};
        }
        void Abort() noexcept { mReserved = {}; }
        bool Collect(uint64_t completed) noexcept
        {
            if (!mSubmission || mSubmission > completed)
                return false;
            if (mFlight.Generation == mGeneration)
                mCompletedRows += mFlight.Rows;
            else
                ++mStale;
            mSubmission = 0;
            return true;
        }
        [[nodiscard]] uint64_t Generation() const noexcept { return mGeneration; }
        [[nodiscard]] uint64_t Submitted() const noexcept { return mHeight ? mSubmittedRows / mHeight : 0; }
        [[nodiscard]] uint64_t Completed() const noexcept { return mHeight ? mCompletedRows / mHeight : 0; }
        [[nodiscard]] double Average() const noexcept { return mHeight ? double(mCompletedRows) / mHeight : 0; }
        [[nodiscard]] uint64_t Maximum() const noexcept { return mHeight ? (mCompletedRows + mHeight - 1) / mHeight : 0; }
        [[nodiscard]] bool InFlight() const noexcept { return mSubmission != 0; }
        [[nodiscard]] uint64_t Stale() const noexcept { return mStale; }
    private:
        uint64_t mGeneration = 0, mSubmission = 0, mSubmittedRows = 0, mCompletedRows = 0, mStale = 0;
        uint32_t mHeight = 1;
        Band mReserved, mFlight;
    };
}
