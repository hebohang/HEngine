#pragma once

#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <cstdint>
#include <mutex>

namespace HEngine
{
    // Owns immutable frame snapshots between ECS extraction and rendering.
    // The queue never exposes a command list and does not keep references to
    // registry storage, so render passes cannot query or mutate ECS state.
    class RenderWorldSubmission final
    {
    public:
        void Submit(RenderWorld world);
        [[nodiscard]] Ref<const RenderWorld> AcquireLatest() const;
        void Reset();

        [[nodiscard]] uint64_t GetLatestFrameNumber() const;
        [[nodiscard]] bool HasSubmission() const;

    private:
        mutable std::mutex mMutex;
        Ref<const RenderWorld> mLatest;
        uint64_t mLatestFrameNumber = 0;
    };
}
