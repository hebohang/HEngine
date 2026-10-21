#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>

namespace HEngine
{
    // The caller holds the preparation queue mutex. Active includes waiting
    // and worker-owned jobs; cancelling a waiting job frees capacity now.
    template<typename Job>
    class TexturePreparationQueueState final
    {
    public:
        explicit TexturePreparationQueueState(uint64_t preparedByteBudget =
            128ull * 1024 * 1024) noexcept
            : mPreparedByteBudget(preparedByteBudget) {}

        [[nodiscard]] bool TryEnqueue(std::shared_ptr<Job> job, size_t maximumActive)
        {
            if (!job) throw std::invalid_argument("Texture preparation job is null");
            if (mActive >= maximumActive || mPreparedBytes >= mPreparedByteBudget)
                return false;
            mWaiting.push_back(std::move(job));
            ++mActive;
            return true;
        }

        [[nodiscard]] std::shared_ptr<Job> TakeNext()
        {
            if (mWaiting.empty()) throw std::logic_error("No waiting texture job");
            auto job = std::move(mWaiting.front());
            mWaiting.pop_front();
            return job;
        }

        [[nodiscard]] bool CancelWaiting(const std::shared_ptr<Job>& job) noexcept
        {
            const auto found = std::find(mWaiting.begin(), mWaiting.end(), job);
            if (found == mWaiting.end()) return false;
            mWaiting.erase(found);
            --mActive;
            return true;
        }

        // A blocking owner request can remove its queued job and prepare it
        // synchronously without waiting behind this queue's byte gate.
        [[nodiscard]] bool TakeWaitingForOwner(const std::shared_ptr<Job>& job) noexcept
        { return CancelWaiting(job); }

        void CompleteActive()
        {
            if (mActive == 0) throw std::logic_error("No active texture job");
            --mActive;
        }

        [[nodiscard]] bool CanStart(size_t frameCredits) const noexcept
        { return !mWaiting.empty() && frameCredits != 0 &&
            mPreparedBytes < mPreparedByteBudget; }

        void AddPreparedBytes(uint64_t bytes) noexcept { mPreparedBytes += bytes; }
        void ReleasePreparedBytes(uint64_t bytes) noexcept
        { mPreparedBytes -= std::min(mPreparedBytes, bytes); }
        void RecordBudgetStall() noexcept { ++mDecodeBudgetStalls; }

        [[nodiscard]] bool Empty() const noexcept { return mWaiting.empty(); }
        [[nodiscard]] size_t Active() const noexcept { return mActive; }
        [[nodiscard]] size_t Waiting() const noexcept { return mWaiting.size(); }
        [[nodiscard]] std::shared_ptr<Job> PeekWaiting() const noexcept
        { return mWaiting.empty() ? nullptr : mWaiting.front(); }
        [[nodiscard]] uint64_t PreparedBytes() const noexcept { return mPreparedBytes; }
        [[nodiscard]] uint64_t PreparedByteBudget() const noexcept
        { return mPreparedByteBudget; }
        [[nodiscard]] uint64_t DecodeBudgetStalls() const noexcept
        { return mDecodeBudgetStalls; }

    private:
        std::deque<std::shared_ptr<Job>> mWaiting;
        size_t mActive = 0;
        uint64_t mPreparedBytes = 0;
        uint64_t mPreparedByteBudget = 0;
        uint64_t mDecodeBudgetStalls = 0;
    };
}
