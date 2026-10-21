#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace HEngine
{
    // Owner-thread stages express ordering without moving ECS, Jolt, script,
    // UI, or RHI work to CPU workers. A stage may join a separate TaskGraph
    // for an audited pure-computation partition before its successors run.
    class FrameStageGraph final
    {
    public:
        struct Handle
        {
            uint64_t GraphId = 0;
            uint32_t Index = 0;
        };

        FrameStageGraph()
            : mGraphId(sNextGraphId.fetch_add(1, std::memory_order_relaxed))
            , mOwner(std::this_thread::get_id())
        {
            if (mGraphId == 0)
                throw std::overflow_error("Frame stage identity exhausted");
        }
        FrameStageGraph(const FrameStageGraph&) = delete;
        FrameStageGraph& operator=(const FrameStageGraph&) = delete;
        FrameStageGraph(FrameStageGraph&&) = delete;
        FrameStageGraph& operator=(FrameStageGraph&&) = delete;

        [[nodiscard]] Handle Add(std::string_view name, std::function<void()> work)
        {
            CheckEditable();
            if (!work) throw std::invalid_argument("Frame stage has no work");
            if (mCount == mNodes.size())
                throw std::length_error("Frame stage capacity exceeded");
            const uint32_t index = mCount;
            mNodes[index] = Node{name, std::move(work), 0};
            ++mCount;
            return {mGraphId, index};
        }

        void After(Handle child, Handle prerequisite)
        {
            CheckEditable();
            if (child.GraphId != mGraphId || prerequisite.GraphId != mGraphId ||
                child.Index >= mCount || prerequisite.Index >= mCount ||
                child.Index == prerequisite.Index)
                throw std::invalid_argument("Invalid frame stage dependency");
            mNodes[child.Index].Prerequisites |= uint32_t{1} << prerequisite.Index;
        }

        void Execute()
        {
            CheckEditable();
            mExecuted = true;
            std::array<uint32_t, 32> order{};
            uint32_t pending = mCount == 32 ? ~uint32_t{0} :
                (uint32_t{1} << mCount) - 1u;
            for (uint32_t position = 0; position < mCount; ++position)
            {
                uint32_t ready = mCount;
                for (uint32_t index = 0; index < mCount; ++index)
                    if ((pending & (uint32_t{1} << index)) != 0 &&
                        (mNodes[index].Prerequisites & pending) == 0)
                    {
                        ready = index;
                        break;
                    }
                // Validate the complete DAG before invoking an external stage.
                if (ready == mCount)
                    throw std::invalid_argument("Frame stage graph contains a cycle");
                order[position] = ready;
                pending &= ~(uint32_t{1} << ready);
            }
            for (uint32_t position = 0; position < mCount; ++position)
                mNodes[order[position]].Work();
        }

    private:
        struct Node
        {
            std::string_view Name;
            std::function<void()> Work;
            uint32_t Prerequisites = 0;
        };

        void CheckEditable() const
        {
            if (std::this_thread::get_id() != mOwner)
                throw std::logic_error("Frame stages require their owner thread");
            if (mExecuted)
                throw std::logic_error("Frame stage graph has already run");
        }

        std::array<Node, 32> mNodes{};
        inline static std::atomic_uint64_t sNextGraphId{1};
        uint64_t mGraphId;
        uint32_t mCount = 0;
        std::thread::id mOwner;
        bool mExecuted = false;
    };
}
