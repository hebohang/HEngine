#pragma once

#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <unordered_map>

namespace HEngine
{
    // Each view advances motion history only after a successful submission.
    class SubmittedMotionHistory
    {
    public:
        void Stage(Ref<const RenderWorld> world)
        {
            mPending = std::move(world);
            mPendingIndices.clear();
            if (!mPending) return;
            mPendingIndices.reserve(mPending->Objects.size());
            for (size_t index = 0; index < mPending->Objects.size(); ++index)
                mPendingIndices.emplace(Key(mPending->Objects[index]), index);
        }
        void Commit() noexcept
        {
            if (!mPending) return;
            mSubmitted = std::move(mPending);
            mSubmittedIndices.swap(mPendingIndices);
            mPendingIndices.clear();
        }
        void Abort() noexcept { mPending.reset(); mPendingIndices.clear(); }
        void Reset() noexcept { Abort(); mSubmitted.reset(); mSubmittedIndices.clear(); }
        [[nodiscard]] bool HasSubmittedScene(uint64_t generation) const noexcept
        {
            return mSubmitted && mSubmitted->SceneGeneration == generation;
        }
        [[nodiscard]] const RenderObject* Previous(const RenderWorld& current,
            const RenderObject& object, bool bHistoryReset) const noexcept
        {
            if (bHistoryReset || current.Camera.bCameraCut || !mSubmitted ||
                current.SceneGeneration != mSubmitted->SceneGeneration ||
                object.EntityId < 0 || object.ObjectGpuIndex == InvalidGpuSceneIndex)
                return nullptr;
            const auto found = mSubmittedIndices.find(Key(object));
            if (found == mSubmittedIndices.end()) return nullptr;
            const auto& previous = mSubmitted->Objects[found->second];
            if (previous.MeshResource != object.MeshResource ||
                previous.MeshAssetPath != object.MeshAssetPath || previous.Path != object.Path)
                return nullptr;
            return &previous;
        }
    private:
        [[nodiscard]] static uint64_t Key(const RenderObject& object) noexcept
        {
            return (uint64_t(uint32_t(object.EntityId)) << 32u) | object.ObjectGpuIndex;
        }
        Ref<const RenderWorld> mSubmitted;
        Ref<const RenderWorld> mPending;
        std::unordered_map<uint64_t, size_t> mSubmittedIndices;
        std::unordered_map<uint64_t, size_t> mPendingIndices;
    };
}
