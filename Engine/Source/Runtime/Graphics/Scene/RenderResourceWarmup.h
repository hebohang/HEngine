#pragma once

#include <cstdint>

namespace HEngine
{
    enum class RenderResourceWarmupState : uint8_t { Pending, Ready, TimedOut };

    // Completion is based on distinct rendered frames after all queued uploads
    // and graph changes have drained, never on an arbitrary startup frame count.
    class RenderResourceWarmup final
    {
    public:
        void Begin(double nowSeconds, uint64_t previousWorldFrame) noexcept
        {
            mStartSeconds = nowSeconds;
            mLastWorldFrame = previousWorldFrame;
            mStableFrames = 0;
        }

        [[nodiscard]] RenderResourceWarmupState Observe(bool bResourcesReady,
            uint64_t worldFrame, double nowSeconds) noexcept
        {
            if (nowSeconds - mStartSeconds >= 90.0)
                return RenderResourceWarmupState::TimedOut;
            if (worldFrame != mLastWorldFrame)
            {
                mLastWorldFrame = worldFrame;
                mStableFrames = bResourcesReady ? mStableFrames + 1 : 0;
            }
            else if (!bResourcesReady)
                mStableFrames = 0;
            return mStableFrames >= 3 ? RenderResourceWarmupState::Ready
                                      : RenderResourceWarmupState::Pending;
        }

    private:
        double mStartSeconds = 0.0;
        uint64_t mLastWorldFrame = 0;
        uint32_t mStableFrames = 0;
    };
}
