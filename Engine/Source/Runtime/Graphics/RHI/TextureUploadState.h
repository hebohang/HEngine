#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace HEngine
{
    struct TextureLatencyPercentiles
    {
        size_t Samples = 0;
        double P50Milliseconds = 0.0;
        double P95Milliseconds = 0.0;
        double P99Milliseconds = 0.0;
        double MaxMilliseconds = 0.0;
    };

    // Owner-thread-only rolling request-to-publication latency. The fixed
    // storage avoids allocating while publishing GPU resources.
    class TextureCompletionLatencyWindow final
    {
    public:
        static constexpr size_t Capacity = 256;

        void Add(double milliseconds) noexcept
        {
            if (!std::isfinite(milliseconds) || milliseconds < 0.0) return;
            mValues[mNext] = milliseconds;
            mNext = (mNext + 1) % Capacity;
            mCount = std::min(mCount + 1, Capacity);
            auto ordered = mValues;
            std::sort(ordered.begin(), ordered.begin() + mCount);
            const auto at = [&](double quantile) noexcept -> double
            { return ordered[static_cast<size_t>(std::ceil(mCount * quantile)) - 1]; };
            mPercentiles = { mCount, at(0.50), at(0.95), at(0.99),
                ordered[mCount - 1] };
        }

        [[nodiscard]] TextureLatencyPercentiles Get() const noexcept
        { return mPercentiles; }

    private:
        std::array<double, Capacity> mValues{};
        size_t mNext = 0;
        size_t mCount = 0;
        TextureLatencyPercentiles mPercentiles;
    };

    enum class TextureUploadStage
    {
        Empty,
        Preparing,
        Submitted,
        Ready,
        Failed,
        Cancelled,
    };

    // Owner-thread state transition contract; GPU queries and CPU jobs supply
    // the evidence for Submit and Complete respectively.
    class TextureUploadState final
    {
    public:
        [[nodiscard]] TextureUploadStage Get() const noexcept { return mStage; }
        void BeginPreparation()
        {
            Require(TextureUploadStage::Empty);
            mStage = TextureUploadStage::Preparing;
        }
        void BeginReload()
        {
            Require(TextureUploadStage::Ready);
            mStage = TextureUploadStage::Preparing;
        }
        void Submit()
        {
            if (mStage != TextureUploadStage::Empty &&
                mStage != TextureUploadStage::Preparing)
                throw std::logic_error("Texture upload cannot be submitted from this stage");
            mStage = TextureUploadStage::Submitted;
        }
        void Complete()
        {
            Require(TextureUploadStage::Submitted);
            mStage = TextureUploadStage::Ready;
        }
        void Fail()
        {
            if (mStage != TextureUploadStage::Preparing &&
                mStage != TextureUploadStage::Submitted)
                throw std::logic_error("Texture upload cannot fail from this stage");
            mStage = TextureUploadStage::Failed;
        }
        void Retry()
        {
            Require(TextureUploadStage::Failed);
            mStage = TextureUploadStage::Preparing;
        }
        void Cancel() noexcept { mStage = TextureUploadStage::Cancelled; }
        void Reset()
        {
            if (mStage != TextureUploadStage::Empty &&
                mStage != TextureUploadStage::Cancelled)
                throw std::logic_error("Live texture upload cannot be reset");
            mStage = TextureUploadStage::Empty;
        }

    private:
        void Require(TextureUploadStage expected) const
        {
            if (mStage != expected)
                throw std::logic_error("Invalid texture upload state transition");
        }
        TextureUploadStage mStage = TextureUploadStage::Empty;
    };
}
