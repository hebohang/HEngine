#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    struct TaskHandle
    {
        size_t Index = size_t(-1);
        uint64_t GraphId = 0;
    };
    enum class TaskPriority : uint8_t { Background, Normal, FrameCritical };

    // Build edges before Execute. A graph's tasks must not wait on other tasks;
    // dependencies are expressed through After.
    class TaskGraph final
    {
    public:
        using Function = std::function<void()>;
        TaskGraph();
        [[nodiscard]] TaskHandle Add(std::string name, Function function,
            TaskPriority priority = TaskPriority::Normal);
        void After(TaskHandle child, TaskHandle prerequisite);
        void Cancel() noexcept { mCancelled.store(true, std::memory_order_release); }
        [[nodiscard]] bool IsCancelled() const noexcept
        { return mCancelled.load(std::memory_order_acquire); }
        [[nodiscard]] size_t Size() const noexcept { return mNodes.size(); }

    private:
        struct Node
        {
            std::string Name;
            Function Work;
            TaskPriority Priority;
            std::vector<size_t> Prerequisites;
        };
        std::vector<Node> mNodes;
        uint64_t mGraphId = 0;
        std::atomic_bool mCancelled{false};
        std::atomic_bool mSealed{false};
        friend class TaskScheduler;
    };

    struct TaskSchedulerStats
    {
        uint64_t Tasks = 0;
        double QueuedMilliseconds = 0.0;
        double ExecutedMilliseconds = 0.0;
        double WallMilliseconds = 0.0;
        double CriticalPathMilliseconds = 0.0;
        double SchedulingMilliseconds = 0.0;
        double WorkerUtilization = 0.0;
    };

    class TaskScheduler final
    {
    public:
        explicit TaskScheduler(size_t workerCount, size_t maximumQueuedTasks = 256);
        ~TaskScheduler();
        TaskScheduler(const TaskScheduler&) = delete;
        TaskScheduler& operator=(const TaskScheduler&) = delete;

        // Independent DAGs may execute concurrently. Frame-critical jobs use
        // reserved queue space and run before already queued lower-priority jobs.
        // Returns at this graph's barrier and propagates its first task exception.
        [[nodiscard]] TaskSchedulerStats Execute(TaskGraph& graph);
        template<typename F>
        [[nodiscard]] TaskSchedulerStats ParallelFor(
            std::string_view name, size_t count, size_t batchSize, F&& function,
            TaskPriority priority = TaskPriority::FrameCritical)
        {
            TaskGraph graph;
            if (batchSize == 0) throw std::invalid_argument("ParallelFor batch size is zero");
            for (size_t begin = 0; begin < count;)
            {
                const size_t end = begin + std::min(batchSize, count - begin);
                (void)graph.Add(std::string(name), [begin, end, &function]
                {
                    for (size_t i = begin; i < end; ++i) function(i);
                }, priority);
                begin = end;
            }
            return Execute(graph);
        }
        [[nodiscard]] size_t WorkerCount() const noexcept;
        [[nodiscard]] size_t QueuedTaskCount() const;
        // A task may request stop; a non-task caller completes the join.
        void Shutdown() noexcept;

    private:
        struct Impl;
        Impl* mImpl;
    };

    // Shared CPU budget for frame snapshot work and background CPU preparation.
    // HE_TASKS_SERIAL=1 selects the deterministic single-thread path.
    [[nodiscard]] TaskScheduler& GetRuntimeTaskScheduler();
}
