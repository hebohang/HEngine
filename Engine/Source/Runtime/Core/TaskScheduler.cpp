#include "Hepch.h"
#include "Runtime/Core/TaskScheduler.h"
#include "Runtime/Core/RuntimeCpuBudget.h"

#include <chrono>
#include <array>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

namespace HEngine
{
    namespace
    {
        thread_local bool sInsideTask = false;
        thread_local const void* sCurrentScheduler = nullptr;
        std::atomic_uint64_t sNextTaskGraphId{1};
    }

    TaskGraph::TaskGraph()
        : mGraphId(sNextTaskGraphId.fetch_add(1, std::memory_order_relaxed))
    {
        if (mGraphId == 0)
            throw std::overflow_error("Task graph identity exhausted");
    }

    TaskHandle TaskGraph::Add(std::string name, Function function, TaskPriority priority)
    {
        if (mSealed.load(std::memory_order_acquire))
            throw std::logic_error("Cannot add tasks after graph execution");
        if (!function) throw std::invalid_argument("Task has no function");
        if (static_cast<size_t>(priority) >= 3)
            throw std::invalid_argument("Unknown task priority");
        if (mNodes.size() >= 8192) throw std::length_error("Task graph capacity exceeded");
        const TaskHandle handle{mNodes.size(), mGraphId};
        mNodes.push_back({std::move(name), std::move(function), priority, {}});
        return handle;
    }

    void TaskGraph::After(TaskHandle child, TaskHandle prerequisite)
    {
        if (mSealed.load(std::memory_order_acquire))
            throw std::logic_error("Cannot add dependencies after graph execution");
        if (child.GraphId != mGraphId || prerequisite.GraphId != mGraphId ||
            child.Index >= mNodes.size() || prerequisite.Index >= mNodes.size() ||
            child.Index == prerequisite.Index)
            throw std::invalid_argument("Invalid task dependency");
        auto& edges = mNodes[child.Index].Prerequisites;
        if (std::find(edges.begin(), edges.end(), prerequisite.Index) == edges.end())
            edges.push_back(prerequisite.Index);
    }

    struct TaskScheduler::Impl
    {
        struct Job
        {
            std::function<void()> Run;
        };
        std::mutex QueueMutex;
        std::condition_variable QueueChanged;
        std::array<std::deque<Job>, 3> Queues;
        size_t QueuedTasks = 0;
        size_t ActiveExecutions = 0;
        std::vector<std::thread> Workers;
        std::atomic_size_t LiveWorkers{0};
        std::mutex SerialExecuteMutex;
        std::mutex ShutdownMutex;
        size_t MaximumQueuedTasks;
        bool Stopping = false;

        Impl(size_t workerCount, size_t maximumQueuedTasks)
            : MaximumQueuedTasks(maximumQueuedTasks)
        {
            if (maximumQueuedTasks == 0)
                throw std::invalid_argument("Task queue capacity is zero");
            Workers.reserve(workerCount);
            try
            {
                for (size_t i = 0; i < workerCount; ++i)
                    Workers.emplace_back([this]
                    {
                        for (;;)
                        {
                            Job job;
                            {
                                std::unique_lock lock(QueueMutex);
                                QueueChanged.wait(lock, [this] { return Stopping || QueuedTasks != 0; });
                                if (QueuedTasks == 0 && Stopping) return;
                                for (auto priority = Queues.rbegin(); priority != Queues.rend(); ++priority)
                                {
                                    if (priority->empty()) continue;
                                    job = std::move(priority->front());
                                    priority->pop_front();
                                    --QueuedTasks;
                                    break;
                                }
                            }
                            QueueChanged.notify_all();
                            job.Run();
                        }
                    });
            }
            catch (...)
            {
                {
                    std::lock_guard lock(QueueMutex);
                    Stopping = true;
                }
                QueueChanged.notify_all();
                for (auto& worker : Workers)
                    if (worker.joinable()) worker.join();
                throw;
            }
            LiveWorkers.store(Workers.size(), std::memory_order_release);
        }

        void Enqueue(Job job, TaskPriority priority)
        {
            const size_t limit = priority == TaskPriority::FrameCritical
                ? MaximumQueuedTasks
                : priority == TaskPriority::Normal
                    ? std::max<size_t>(1, MaximumQueuedTasks - MaximumQueuedTasks / 4)
                    : std::max<size_t>(1, MaximumQueuedTasks / 2);
            std::unique_lock lock(QueueMutex);
            QueueChanged.wait(lock, [this, limit]
            { return Stopping || QueuedTasks < limit; });
            if (Stopping) throw std::runtime_error("Task scheduler is shut down");
            Queues[static_cast<size_t>(priority)].push_back(std::move(job));
            ++QueuedTasks;
            lock.unlock();
            QueueChanged.notify_all();
        }
    };

    TaskScheduler::TaskScheduler(size_t workerCount, size_t maximumQueuedTasks)
        : mImpl(new Impl(workerCount, maximumQueuedTasks)) {}

    TaskScheduler::~TaskScheduler()
    {
        Shutdown();
        delete mImpl;
    }

    size_t TaskScheduler::WorkerCount() const noexcept
    { return mImpl->LiveWorkers.load(std::memory_order_acquire); }

    size_t TaskScheduler::QueuedTaskCount() const
    {
        std::lock_guard lock(mImpl->QueueMutex);
        return mImpl->QueuedTasks;
    }

    void TaskScheduler::Shutdown() noexcept
    {
        // A worker can request shutdown, but joining its own thread must wait
        // until an external caller or the destructor reaches this method.
        if (sCurrentScheduler == mImpl)
        {
            {
                std::lock_guard queueLock(mImpl->QueueMutex);
                mImpl->Stopping = true;
            }
            mImpl->QueueChanged.notify_all();
            return;
        }
        std::lock_guard shutdownLock(mImpl->ShutdownMutex);
        {
            std::unique_lock queueLock(mImpl->QueueMutex);
            mImpl->Stopping = true;
            mImpl->QueueChanged.notify_all();
            mImpl->QueueChanged.wait(queueLock,
                [this] { return mImpl->ActiveExecutions == 0; });
        }
        for (auto& worker : mImpl->Workers)
            if (worker.joinable()) worker.join();
        mImpl->Workers.clear();
        mImpl->LiveWorkers.store(0, std::memory_order_release);
    }

    TaskSchedulerStats TaskScheduler::Execute(TaskGraph& graph)
    {
        if (sInsideTask)
            throw std::logic_error("Task workers cannot wait on a task graph");
        std::unique_lock serialLock(mImpl->SerialExecuteMutex, std::defer_lock);
        if (mImpl->LiveWorkers.load(std::memory_order_acquire) == 0)
            serialLock.lock();
        {
            std::lock_guard queueLock(mImpl->QueueMutex);
            if (mImpl->Stopping) throw std::runtime_error("Task scheduler is shut down");
            if (graph.mSealed.exchange(true, std::memory_order_acq_rel))
                throw std::logic_error("Task graph has already run");
            ++mImpl->ActiveExecutions;
        }
        struct ExecutionScope
        {
            Impl* Scheduler;
            ~ExecutionScope()
            {
                {
                    std::lock_guard lock(Scheduler->QueueMutex);
                    --Scheduler->ActiveExecutions;
                }
                Scheduler->QueueChanged.notify_all();
            }
        } executionScope{mImpl};
        const auto wallStart = std::chrono::steady_clock::now();
        const size_t count = graph.mNodes.size();
        std::vector<std::vector<size_t>> dependents(count);
        std::vector<size_t> remaining(count);
        for (size_t child = 0; child < count; ++child)
        {
            remaining[child] = graph.mNodes[child].Prerequisites.size();
            for (size_t prerequisite : graph.mNodes[child].Prerequisites)
                dependents[prerequisite].push_back(child);
        }
        // Reject cycles before publishing any task to a worker.
        auto auditRemaining = remaining;
        std::deque<size_t> audit;
        for (size_t i = 0; i < count; ++i)
            if (auditRemaining[i] == 0) audit.push_back(i);
        size_t visited = 0;
        while (!audit.empty())
        {
            const size_t current = audit.front();
            audit.pop_front();
            ++visited;
            for (size_t child : dependents[current])
                if (--auditRemaining[child] == 0) audit.push_back(child);
        }
        if (visited != count) throw std::invalid_argument("Task graph contains a cycle");

        std::mutex stateMutex;
        std::condition_variable stateChanged;
        std::vector<size_t> ready;
        ready.reserve(count);
        for (size_t i = 0; i < count; ++i)
            if (remaining[i] == 0) ready.push_back(i);
        size_t running = 0;
        std::exception_ptr failure;
        TaskSchedulerStats stats;
        std::vector<double> taskDurations(count, 0.0);
        std::vector<size_t> previousOnWorker(count, size_t(-1));
        std::vector<size_t> executionOrder;
        executionOrder.reserve(count);
        std::vector<std::pair<std::thread::id, size_t>> workerLanes;
        workerLanes.reserve(std::max<size_t>(1, mImpl->Workers.size()));
        auto finish = [&](size_t index, double queuedMs, double executedMs,
            std::exception_ptr error)
        {
            {
                std::lock_guard lock(stateMutex);
                ++stats.Tasks;
                taskDurations[index] = executedMs;
                stats.QueuedMilliseconds += queuedMs;
                stats.ExecutedMilliseconds += executedMs;
                if (error && !failure)
                {
                    failure = error;
                    graph.Cancel();
                }
                if (!graph.IsCancelled())
                    for (size_t child : dependents[index])
                        if (--remaining[child] == 0) ready.push_back(child);
                --running;
            }
            stateChanged.notify_all();
        };

        for (;;)
        {
            size_t index = size_t(-1);
            {
                std::unique_lock lock(stateMutex);
                stateChanged.wait(lock, [&]
                { return graph.IsCancelled() || !ready.empty() || running == 0; });
                if (graph.IsCancelled())
                {
                    if (running == 0) break;
                    stateChanged.wait(lock, [&] { return running == 0; });
                    break;
                }
                if (ready.empty())
                {
                    if (running == 0) break;
                    continue;
                }
                const auto best = std::max_element(ready.begin(), ready.end(),
                    [&](size_t a, size_t b)
                    { return graph.mNodes[a].Priority < graph.mNodes[b].Priority; });
                index = *best;
                ready.erase(best);
                ++running;
            }
            const auto enqueued = std::chrono::steady_clock::now();
            auto run = [&, index, enqueued]
            {
                {
                    std::lock_guard lock(stateMutex);
                    const auto threadId = std::this_thread::get_id();
                    const auto lane = std::find_if(workerLanes.begin(), workerLanes.end(),
                        [&](const auto& entry) { return entry.first == threadId; });
                    if (lane == workerLanes.end())
                        workerLanes.emplace_back(threadId, index);
                    else
                    {
                        previousOnWorker[index] = lane->second;
                        lane->second = index;
                    }
                    executionOrder.push_back(index);
                }
                const auto started = std::chrono::steady_clock::now();
                std::exception_ptr error;
                if (!graph.IsCancelled())
                {
                    try
                    {
                        struct TaskScope
                        {
                            const void* PreviousScheduler;
                            bool PreviousInsideTask;
                            explicit TaskScope(const void* scheduler)
                                : PreviousScheduler(sCurrentScheduler)
                                , PreviousInsideTask(sInsideTask)
                            {
                                sCurrentScheduler = scheduler;
                                sInsideTask = true;
                            }
                            ~TaskScope()
                            {
                                sCurrentScheduler = PreviousScheduler;
                                sInsideTask = PreviousInsideTask;
                            }
                        } taskScope(mImpl);
                        HE_PROFILE_TRANSIENT_SECTION(graph.mNodes[index].Name.c_str());
                        graph.mNodes[index].Work();
                    }
                    catch (...) { error = std::current_exception(); }
                }
                const auto ended = std::chrono::steady_clock::now();
                finish(index,
                    std::chrono::duration<double, std::milli>(started - enqueued).count(),
                    std::chrono::duration<double, std::milli>(ended - started).count(),
                    error);
            };
            if (mImpl->Workers.empty()) run();
            else
            {
                try { mImpl->Enqueue({std::move(run)}, graph.mNodes[index].Priority); }
                catch (...)
                {
                    graph.Cancel();
                    std::unique_lock lock(stateMutex);
                    --running; // This node was never published.
                    stateChanged.wait(lock, [&] { return running == 0; });
                    throw;
                }
            }
        }
        stats.WallMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - wallStart).count();
        std::vector<double> longestPath(count, 0.0);
        for (size_t index : executionOrder)
        {
            double longestPrerequisite = 0.0;
            for (size_t prerequisite : graph.mNodes[index].Prerequisites)
                longestPrerequisite = std::max(longestPrerequisite, longestPath[prerequisite]);
            if (previousOnWorker[index] != size_t(-1))
                longestPrerequisite = std::max(
                    longestPrerequisite, longestPath[previousOnWorker[index]]);
            longestPath[index] = longestPrerequisite + taskDurations[index];
            stats.CriticalPathMilliseconds = std::max(
                stats.CriticalPathMilliseconds, longestPath[index]);
        }
        stats.SchedulingMilliseconds = std::max(0.0,
            stats.WallMilliseconds - stats.CriticalPathMilliseconds);
        if (!mImpl->Workers.empty() && stats.WallMilliseconds > 0.0)
            stats.WorkerUtilization = std::min(1.0,
                stats.ExecutedMilliseconds /
                    (stats.WallMilliseconds * mImpl->Workers.size()));
        static const bool bLogTimings = []
        {
            const char* value = std::getenv("HE_TASK_TIMINGS");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        if (bLogTimings && stats.Tasks != 0)
            HE_CORE_INFO("[TaskScheduler] tasks={} queueMs={:.3f} executeMs={:.3f} wallMs={:.3f} criticalMs={:.3f} schedulingMs={:.3f} workerUtilization={:.3f}",
                stats.Tasks, stats.QueuedMilliseconds,
                stats.ExecutedMilliseconds, stats.WallMilliseconds,
                stats.CriticalPathMilliseconds, stats.SchedulingMilliseconds,
                stats.WorkerUtilization);
        if (failure) std::rethrow_exception(failure);
        return stats;
    }

    TaskScheduler& GetRuntimeTaskScheduler()
    {
        static TaskScheduler scheduler([]
        {
            const char* serial = std::getenv("HE_TASKS_SERIAL");
            if (serial && serial[0] == '1' && serial[1] == '\0') return size_t(0);
            return GetRuntimeCpuBudget().TaskWorkers;
        }(), 32);
        return scheduler;
    }
}
