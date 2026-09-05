#include "Hepch.h"

#include "Runtime/Graphics/Scene/RenderWorldSubmission.h"

#include <stdexcept>
#include <utility>

namespace HEngine
{
    void RenderWorldSubmission::Submit(RenderWorld world)
    {
        std::scoped_lock lock(mMutex);
        if (mLatest && world.FrameNumber < mLatestFrameNumber)
            throw std::logic_error("RenderWorld submissions must not move backwards in frame number");

        mLatestFrameNumber = world.FrameNumber;
        mLatest = CreateRef<const RenderWorld>(std::move(world));
    }

    Ref<const RenderWorld> RenderWorldSubmission::AcquireLatest() const
    {
        std::scoped_lock lock(mMutex);
        return mLatest;
    }

    void RenderWorldSubmission::Reset()
    {
        std::scoped_lock lock(mMutex);
        mLatest.reset();
        mLatestFrameNumber = 0;
    }

    uint64_t RenderWorldSubmission::GetLatestFrameNumber() const
    {
        std::scoped_lock lock(mMutex);
        return mLatestFrameNumber;
    }

    bool RenderWorldSubmission::HasSubmission() const
    {
        std::scoped_lock lock(mMutex);
        return mLatest != nullptr;
    }
}
