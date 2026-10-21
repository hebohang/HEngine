#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>

namespace HEngine
{
    struct RuntimeCpuBudget
    {
        size_t ResourceWorkers = 0;
        size_t TextureWorkers = 1;
        size_t UiDecodeWorkers = 0;
        size_t TaskWorkers = 0;
        int JoltWorkers = 0;
    };

    // Resource and texture workers may block on IO. Keep both available even
    // on one or two logical processors, but run Jolt and CPU tasks on the
    // caller there. On larger machines, reserve the main thread first.
    [[nodiscard]] constexpr RuntimeCpuBudget PlanRuntimeCpuBudget(
        unsigned reportedLogicalCores) noexcept
    {
        const unsigned cores = reportedLogicalCores == 0 ? 4 : reportedLogicalCores;
        RuntimeCpuBudget budget;
        budget.ResourceWorkers = cores >= 6 ? 2 : 1;
        budget.UiDecodeWorkers = cores >= 8 ? 1 : 0;
        budget.TaskWorkers = cores <= 4 ? 0 : std::min<size_t>(3, cores / 4);
        const size_t reserved = 1 + budget.ResourceWorkers +
            budget.TextureWorkers + budget.UiDecodeWorkers + budget.TaskWorkers;
        const unsigned available = cores > reserved
            ? static_cast<unsigned>(cores - reserved) : 0;
        const unsigned joltLimit = cores > 7 ? cores - 7 : 1;
        budget.JoltWorkers = static_cast<int>(std::min(available, joltLimit));
        return budget;
    }

    [[nodiscard]] inline RuntimeCpuBudget GetRuntimeCpuBudget() noexcept
    { return PlanRuntimeCpuBudget(std::thread::hardware_concurrency()); }
}
