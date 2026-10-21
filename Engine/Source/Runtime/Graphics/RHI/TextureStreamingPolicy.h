#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace HEngine
{
    struct TextureStreamingDemand
    {
        uint32_t Width = 1, Height = 1;
        uint32_t BlockSize = 1, BytesPerBlock = 4;
        uint32_t DesiredMip = 0;
        float Priority = 1;
    };

    [[nodiscard]] inline uint32_t TextureLastMip(uint32_t width, uint32_t height)
    {
        uint32_t mip = 0;
        for (uint32_t size = std::max(width, height); size > 1; size >>= 1) ++mip;
        return mip;
    }

    [[nodiscard]] inline uint64_t TextureMipTailBytes(const TextureStreamingDemand& demand, uint32_t firstMip)
    {
        const uint32_t last = TextureLastMip(demand.Width, demand.Height);
        uint64_t bytes = 0;
        const uint32_t block = std::max(1u, demand.BlockSize);
        for (uint32_t mip = std::min(firstMip, last); mip <= last; ++mip)
        {
            const uint64_t width = std::max(1u, demand.Width >> mip);
            const uint64_t height = std::max(1u, demand.Height >> mip);
            bytes += ((width + block - 1) / block) * ((height + block - 1) / block) * demand.BytesPerBlock;
        }
        return bytes;
    }

    struct TextureStreamingPlan
    {
        std::vector<uint32_t> Mips;
        uint64_t Bytes = 0;
        bool bMinimumExceedsBudget = false;
    };

    // Greedily remove the least important detail per byte saved. Stable input
    // order breaks ties. Always retain a valid mip tail, even below the minimum budget.
    [[nodiscard]] inline TextureStreamingPlan PlanTextureResidency(
        std::span<const TextureStreamingDemand> demands, uint64_t budget)
    {
        TextureStreamingPlan plan;
        for (const auto& demand : demands)
        {
            plan.Mips.push_back(std::min(demand.DesiredMip, TextureLastMip(demand.Width, demand.Height)));
            plan.Bytes += TextureMipTailBytes(demand, plan.Mips.back());
        }
        while (plan.Bytes > budget)
        {
            size_t best = demands.size();
            double bestCost = INFINITY;
            uint64_t saved = 0;
            for (size_t i = 0; i < demands.size(); ++i)
            {
                const auto& demand = demands[i];
                if (plan.Mips[i] == TextureLastMip(demand.Width, demand.Height)) continue;
                const uint64_t reduction = TextureMipTailBytes(demand, plan.Mips[i]) -
                    TextureMipTailBytes(demand, plan.Mips[i] + 1);
                if (reduction == 0) continue;
                const double priority = std::isfinite(demand.Priority) ? std::max(.001f, demand.Priority) : 1;
                const double cost = priority * std::exp2(double(plan.Mips[i] -
                    std::min(demand.DesiredMip, TextureLastMip(demand.Width, demand.Height)))) / double(reduction);
                if (cost < bestCost) { best = i; bestCost = cost; saved = reduction; }
            }
            if (best == demands.size()) { plan.bMinimumExceedsBudget = true; break; }
            ++plan.Mips[best]; plan.Bytes -= saved;
        }
        return plan;
    }
}
