#include "Hepch.h"
#include "Runtime/UnitTest/TaskSchedulerTests.h"
#include "Runtime/Core/FrameStageGraph.h"
#include "Runtime/Core/TaskScheduler.h"
#include "Runtime/Core/RuntimeCpuBudget.h"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

namespace HEngine::Tests
{
    void TaskSchedulerContract()
    {
        {
            FrameStageGraph stages;
            TaskScheduler partitions(2, 4);
            int state = 0;
            const auto render = stages.Add("RenderPrepare", [&]
            {
                if (state != 7) throw std::runtime_error("RenderPrepare ran early");
                state = 8;
            });
            const auto physics = stages.Add("Physics", [&]
            {
                if (state != 1) throw std::runtime_error("Physics ran early");
                state = 2;
            });
            const auto pre = stages.Add("PrePhysics", [&] { state = 1; });
            const auto display = stages.Add("DisplaySystems", [&]
            {
                if (state != 2) throw std::runtime_error("Display ran early");
                std::array<int, 5> values{};
                (void)partitions.ParallelFor("stage partition", values.size(), 2,
                    [&](size_t index) { values[index] = static_cast<int>(index + 1); });
                for (const int value : values) state += value == 0 ? 0 : 1;
            });
            stages.After(physics, pre);
            stages.After(display, physics);
            stages.After(render, display);
            stages.Execute();
            if (state != 8) throw std::runtime_error("Frame stage DAG lost work");
            bool rerunRejected = false;
            try { stages.Execute(); }
            catch (const std::logic_error&) { rerunRejected = true; }
            if (!rerunRejected) throw std::runtime_error("Frame stages ran twice");

            FrameStageGraph cycle;
            bool called = false;
            const auto a = cycle.Add("a", [&] { called = true; });
            const auto b = cycle.Add("b", [&] { called = true; });
            cycle.After(a, b);
            cycle.After(b, a);
            bool cycleRejected = false;
            try { cycle.Execute(); }
            catch (const std::invalid_argument&) { cycleRejected = true; }
            if (!cycleRejected || called)
                throw std::runtime_error("Frame stage cycle executed work");
            FrameStageGraph foreign;
            const auto local = foreign.Add("local", [] {});
            bool foreignRejected = false;
            try { foreign.After(local, pre); }
            catch (const std::invalid_argument&) { foreignRejected = true; }
            if (!foreignRejected)
                throw std::runtime_error("Frame stage graph accepted a foreign handle");

            FrameStageGraph failure;
            const auto failing = failure.Add("failure", [] { throw std::runtime_error("stage error"); });
            const auto skipped = failure.Add("skipped", [&] { called = true; });
            failure.After(skipped, failing);
            bool propagated = false;
            try { failure.Execute(); }
            catch (const std::runtime_error&) { propagated = true; }
            if (!propagated || called)
                throw std::runtime_error("Frame stage exception did not stop successors");

            FrameStageGraph ownerOnly;
            (void)ownerOnly.Add("owner", [&] { called = true; });
            std::atomic_bool wrongThreadRejected{false};
            std::thread wrongThread([&]
            {
                try { ownerOnly.Execute(); }
                catch (const std::logic_error&)
                { wrongThreadRejected.store(true, std::memory_order_release); }
            });
            wrongThread.join();
            if (!wrongThreadRejected.load(std::memory_order_acquire) || called)
                throw std::runtime_error("Frame stage graph ran off its owner thread");
            ownerOnly.Execute();
            if (!called)
                throw std::runtime_error("Wrong-thread rejection sealed frame stages");
        }
        struct BudgetCase
        {
            unsigned Cores;
            size_t Resources;
            size_t Tasks;
            int Jolt;
        };
        for (const BudgetCase expected : {
            BudgetCase{0, 1, 0, 1}, BudgetCase{1, 1, 0, 0},
            BudgetCase{2, 1, 0, 0}, BudgetCase{3, 1, 0, 0},
            BudgetCase{4, 1, 0, 1}, BudgetCase{5, 1, 1, 1},
            BudgetCase{6, 2, 1, 1}, BudgetCase{8, 2, 2, 1},
            BudgetCase{16, 2, 3, 8} })
        {
            const RuntimeCpuBudget budget = PlanRuntimeCpuBudget(expected.Cores);
            if (budget.ResourceWorkers != expected.Resources ||
                budget.TaskWorkers != expected.Tasks ||
                budget.JoltWorkers != expected.Jolt || budget.TextureWorkers != 1 ||
                budget.UiDecodeWorkers != (expected.Cores >= 8 ? 1u : 0u))
                throw std::runtime_error("Runtime CPU core budget changed unexpectedly");
            if (expected.Cores >= 3 &&
                1 + budget.ResourceWorkers + budget.TextureWorkers + budget.UiDecodeWorkers +
                    budget.TaskWorkers + budget.JoltWorkers > expected.Cores)
                throw std::runtime_error("Runtime CPU workers oversubscribe a low-core host");
        }
        for (size_t workerCount : {size_t(0), size_t(1), size_t(3)})
        {
            TaskScheduler scheduler(workerCount, 2);
            for (int repeat = 0; repeat < 32; ++repeat)
            {
                TaskGraph graph;
                std::atomic_int completed{0};
                const auto root = graph.Add("root", [&] { completed.fetch_add(1); });
                const auto left = graph.Add("left", [&]
                {
                    if (completed.load() < 1) throw std::runtime_error("Left ran early");
                    completed.fetch_add(1);
                });
                const auto right = graph.Add("right", [&]
                {
                    if (completed.load() < 1) throw std::runtime_error("Right ran early");
                    completed.fetch_add(1);
                });
                const auto join = graph.Add("join", [&]
                {
                    if (completed.load() != 3) throw std::runtime_error("Join ran early");
                    completed.fetch_add(1);
                });
                graph.After(left, root);
                graph.After(right, root);
                graph.After(join, left);
                graph.After(join, right);
                const auto stats = scheduler.Execute(graph);
                if (completed != 4 || stats.Tasks != 4)
                    throw std::runtime_error("DAG fan-out/fan-in lost a task");
                if (stats.CriticalPathMilliseconds > stats.WallMilliseconds + 0.01 ||
                    stats.SchedulingMilliseconds < 0.0 ||
                    stats.WorkerUtilization < 0.0 || stats.WorkerUtilization > 1.0)
                    throw std::runtime_error("Task timing accounting is invalid");
            }
            TaskGraph pressure;
            std::atomic_int rootsDone{0};
            std::atomic_int leavesDone{0};
            std::vector<TaskHandle> leaves;
            leaves.reserve(256);
            for (int i = 0; i < 256; ++i)
            {
                const auto root = pressure.Add("pressure root", [&] { ++rootsDone; });
                const auto leaf = pressure.Add("pressure leaf", [&] { ++leavesDone; });
                pressure.After(leaf, root);
                leaves.push_back(leaf);
            }
            const auto sink = pressure.Add("pressure sink", [&]
            {
                if (rootsDone != 256 || leavesDone != 256)
                    throw std::runtime_error("Pressure sink ran before its leaves");
            });
            for (const TaskHandle leaf : leaves) pressure.After(sink, leaf);
            if (scheduler.Execute(pressure).Tasks != 513)
                throw std::runtime_error("Bounded queue lost pressure graph tasks");

            TaskGraph cancelInFlight;
            std::atomic_bool rootStarted{false};
            std::atomic_bool releaseRoot{false};
            std::atomic_int cancelledLeaves{0};
            const auto blockingRoot = cancelInFlight.Add("cancellation root", [&]
            {
                rootStarted = true;
                while (!releaseRoot.load()) std::this_thread::yield();
            });
            for (int i = 0; i < 128; ++i)
            {
                const auto leaf = cancelInFlight.Add("cancelled leaf", [&]
                    { ++cancelledLeaves; });
                cancelInFlight.After(leaf, blockingRoot);
            }
            std::exception_ptr cancellationError;
            std::thread submitter([&]
            {
                try { (void)scheduler.Execute(cancelInFlight); }
                catch (...) { cancellationError = std::current_exception(); }
            });
            const auto startDeadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(5);
            while (!rootStarted.load() && std::chrono::steady_clock::now() < startDeadline)
                std::this_thread::yield();
            cancelInFlight.Cancel();
            releaseRoot = true;
            submitter.join();
            if (cancellationError) std::rethrow_exception(cancellationError);
            if (!rootStarted || cancelledLeaves != 0)
                throw std::runtime_error("In-flight graph cancellation did not stop descendants");

            std::atomic_int concurrentSubmissions{0};
            std::exception_ptr submissionErrors[2];
            std::thread submitters[2];
            for (int thread = 0; thread < 2; ++thread)
                submitters[thread] = std::thread([&, thread]
                {
                    try
                    {
                        TaskGraph concurrent;
                        for (int i = 0; i < 128; ++i)
                            (void)concurrent.Add("concurrent submit", [&]
                                { ++concurrentSubmissions; });
                        if (scheduler.Execute(concurrent).Tasks != 128)
                            throw std::runtime_error("Concurrent graph lost tasks");
                    }
                    catch (...) { submissionErrors[thread] = std::current_exception(); }
                });
            for (auto& submitter : submitters) submitter.join();
            for (const auto& error : submissionErrors)
                if (error) std::rethrow_exception(error);
            if (concurrentSubmissions != 256)
                throw std::runtime_error("Concurrent graph submissions lost work");

            std::vector<uint64_t> squares(4096);
            const auto batchStats = scheduler.ParallelFor(
                "deterministic squares", squares.size(), 73,
                [&](size_t i) { squares[i] = uint64_t(i) * i; });
            if (batchStats.Tasks < 50)
                throw std::runtime_error("ParallelFor did not create bounded batches");
            for (size_t i = 0; i < squares.size(); ++i)
                if (squares[i] != uint64_t(i) * i)
                    throw std::runtime_error("ParallelFor output changed");
            TaskGraph cycle;
            const auto a = cycle.Add("a", [] {});
            const auto b = cycle.Add("b", [] {});
            cycle.After(a, b);
            cycle.After(b, a);
            bool rejected = false;
            try { (void)scheduler.Execute(cycle); }
            catch (const std::invalid_argument&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Task graph cycle was accepted");

            TaskGraph another;
            const auto foreign = another.Add("foreign", [] {});
            rejected = false;
            try { cycle.After(a, foreign); }
            catch (const std::logic_error&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Sealed task graph accepted a new edge");
            TaskGraph local;
            const auto own = local.Add("own", [] {});
            rejected = false;
            try { local.After(own, foreign); }
            catch (const std::invalid_argument&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Foreign task handle was accepted");

            TaskGraph throwing;
            (void)throwing.Add("throw", [] { throw std::runtime_error("expected"); });
            bool propagated = false;
            try { (void)scheduler.Execute(throwing); }
            catch (const std::runtime_error&) { propagated = true; }
            if (!propagated) throw std::runtime_error("Task exception was lost");

            TaskGraph nested;
            (void)nested.Add("nested rejection", [&]
            {
                TaskGraph inner;
                (void)inner.Add("inner", [] {});
                bool refused = false;
                try { (void)scheduler.Execute(inner); }
                catch (const std::logic_error&) { refused = true; }
                if (!refused) throw std::runtime_error("A task waited for its own graph");
            });
            (void)scheduler.Execute(nested);

            TaskGraph cancelled;
            (void)cancelled.Add("skipped", [] { throw std::runtime_error("cancel failed"); });
            cancelled.Cancel();
            (void)scheduler.Execute(cancelled);
            scheduler.Shutdown();
            bool stopped = false;
            try { TaskGraph graph; (void)scheduler.Execute(graph); }
            catch (const std::runtime_error&) { stopped = true; }
            if (!stopped) throw std::runtime_error("Shutdown accepted a graph");
        }

        TaskGraph invalidPriority;
        bool bRejectedPriority = false;
        try
        {
            (void)invalidPriority.Add("invalid priority", [] {},
                static_cast<TaskPriority>(255));
        }
        catch (const std::invalid_argument&) { bRejectedPriority = true; }
        if (!bRejectedPriority)
            throw std::runtime_error("Invalid task priority was accepted");

        TaskScheduler singlePublication(1, 2);
        TaskGraph runOnce;
        std::atomic_bool taskStarted{false};
        std::atomic_bool releaseTask{false};
        (void)runOnce.Add("single publication", [&]
        {
            taskStarted = true;
            while (!releaseTask.load()) std::this_thread::yield();
        });
        std::exception_ptr firstRunError;
        std::thread firstRunner([&]
        {
            try { (void)singlePublication.Execute(runOnce); }
            catch (...) { firstRunError = std::current_exception(); }
        });
        const auto publicationDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!taskStarted.load() && std::chrono::steady_clock::now() < publicationDeadline)
            std::this_thread::yield();
        bool bRejectedSecondRun = false;
        try { (void)singlePublication.Execute(runOnce); }
        catch (const std::logic_error&) { bRejectedSecondRun = true; }
        releaseTask = true;
        firstRunner.join();
        if (firstRunError) std::rethrow_exception(firstRunError);
        if (!taskStarted || !bRejectedSecondRun)
            throw std::runtime_error("One task graph was published twice concurrently");
        singlePublication.Shutdown();

        TaskScheduler priorityScheduler(1, 4);
        TaskGraph background;
        std::atomic_bool backgroundStarted{false};
        std::atomic_bool releaseBackground{false};
        std::atomic_int completedBackground{0};
        (void)background.Add("blocking background", [&]
        {
            backgroundStarted = true;
            while (!releaseBackground.load()) std::this_thread::yield();
            ++completedBackground;
        }, TaskPriority::Background);
        for (int index = 0; index < 8; ++index)
            (void)background.Add("queued background", [&]
                { ++completedBackground; }, TaskPriority::Background);
        std::exception_ptr backgroundError;
        std::thread backgroundSubmitter([&]
        {
            try { (void)priorityScheduler.Execute(background); }
            catch (...) { backgroundError = std::current_exception(); }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((!backgroundStarted.load() || priorityScheduler.QueuedTaskCount() < 2) &&
            std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        TaskGraph frame;
        std::atomic_int backgroundBeforeFrame{-1};
        (void)frame.Add("frame critical", [&]
            { backgroundBeforeFrame = completedBackground.load(); },
            TaskPriority::FrameCritical);
        std::exception_ptr frameError;
        std::thread frameSubmitter([&]
        {
            try { (void)priorityScheduler.Execute(frame); }
            catch (...) { frameError = std::current_exception(); }
        });
        const auto frameQueuedDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool bFrameQueued = false;
        while (std::chrono::steady_clock::now() < frameQueuedDeadline)
        {
            if (priorityScheduler.QueuedTaskCount() == 3)
            {
                bFrameQueued = true;
                break;
            }
            std::this_thread::yield();
        }
        releaseBackground = true;
        backgroundSubmitter.join();
        frameSubmitter.join();
        if (backgroundError) std::rethrow_exception(backgroundError);
        if (frameError) std::rethrow_exception(frameError);
        if (!bFrameQueued || backgroundBeforeFrame != 1 || completedBackground != 9)
            throw std::runtime_error("Frame-critical graph waited behind queued background work");
        priorityScheduler.Shutdown();

        TaskScheduler serialScheduler(0, 2);
        std::atomic_int activeSerialTasks{0};
        std::atomic_int peakSerialTasks{0};
        std::exception_ptr serialErrors[2];
        std::thread serialSubmitters[2];
        for (int thread = 0; thread < 2; ++thread)
            serialSubmitters[thread] = std::thread([&, thread]
            {
                try
                {
                    TaskGraph graph;
                    for (int index = 0; index < 32; ++index)
                        (void)graph.Add("serial fallback", [&]
                        {
                            const int active = ++activeSerialTasks;
                            peakSerialTasks.store(std::max(peakSerialTasks.load(), active));
                            std::this_thread::yield();
                            --activeSerialTasks;
                        });
                    (void)serialScheduler.Execute(graph);
                }
                catch (...) { serialErrors[thread] = std::current_exception(); }
            });
        for (auto& submitter : serialSubmitters) submitter.join();
        for (const auto& error : serialErrors)
            if (error) std::rethrow_exception(error);
        if (peakSerialTasks != 1)
            throw std::runtime_error("Single-thread fallback ran overlapping graphs");
        serialScheduler.Shutdown();

        TaskScheduler workerShutdown(1, 2);
        TaskGraph stopGraph;
        (void)stopGraph.Add("worker shutdown request", [&] { workerShutdown.Shutdown(); });
        (void)workerShutdown.Execute(stopGraph);
        workerShutdown.Shutdown();
        if (workerShutdown.WorkerCount() != 0)
            throw std::runtime_error("Worker-requested shutdown did not stop the scheduler");
    }
}
