#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace HEngine
{
    // Owner-thread counters summed across all simulation ticks in one presented
    // frame. Animation includes deformation; neither duration is GPU skinning.
    struct AnimationCpuTimings
    {
        double AnimationMilliseconds = 0;
        double DeformationMilliseconds = 0;
        double ClothSkinMilliseconds = 0;
        double ClothSolveMilliseconds = 0;
        double ClothRenderMilliseconds = 0;
        uint64_t InstanceUpdates = 0;
        uint64_t DeformationInstanceUpdates = 0;
        uint64_t DeformationVertexUpdates = 0;
    };
    inline thread_local bool sAnimationCpuTimingEnabled = false;
    inline thread_local AnimationCpuTimings sAnimationCpuTimings;
    inline void BeginAnimationCpuTiming(bool enabled) noexcept
    {
        sAnimationCpuTimingEnabled = enabled;
        sAnimationCpuTimings = {};
    }
    class ScopedAnimationCpuTiming final
    {
    public:
        explicit ScopedAnimationCpuTiming(double& destination) noexcept
            : mDestination(destination), bEnabled(sAnimationCpuTimingEnabled)
        { if (bEnabled) mStart = std::chrono::steady_clock::now(); }
        ~ScopedAnimationCpuTiming()
        {
            if (bEnabled) mDestination += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - mStart).count();
        }
        ScopedAnimationCpuTiming(const ScopedAnimationCpuTiming&) = delete;
        ScopedAnimationCpuTiming& operator=(const ScopedAnimationCpuTiming&) = delete;
    private:
        double& mDestination;
        bool bEnabled;
        std::chrono::steady_clock::time_point mStart;
    };

    enum class RenderFrameCpuPhase : uint8_t
    {
        AcquireImage,
        FrameSlotWait,
        Submit,
        Present,
        GarbageCollect,
        CommandListSetup,
        Count,
    };

    struct RenderFrameCpuTimings
    {
        std::array<double, static_cast<size_t>(RenderFrameCpuPhase::Count)> Milliseconds{};
        [[nodiscard]] double Get(RenderFrameCpuPhase phase) const noexcept
        { return Milliseconds[static_cast<size_t>(phase)]; }
        [[nodiscard]] double TotalMilliseconds() const noexcept
        {
            double total = 0.0;
            for (double milliseconds : Milliseconds) total += milliseconds;
            return total;
        }
    };

    // Current-thread instrumentation, enabled only for explicit render-stat
    // runs. Each phase is a disjoint CPU interval, never a GPU duration. A future
    // render-thread consumer must transfer this value with its completed frame.
    inline thread_local bool sRenderFrameCpuTimingEnabled = false;
    inline thread_local RenderFrameCpuTimings sRenderFrameCpuTimings;
    inline void BeginRenderFrameCpuTiming(bool bEnabled) noexcept
    {
        sRenderFrameCpuTimingEnabled = bEnabled;
        sRenderFrameCpuTimings = {};
    }
    [[nodiscard]] inline RenderFrameCpuTimings EndRenderFrameCpuTiming() noexcept
    {
        sRenderFrameCpuTimingEnabled = false;
        return sRenderFrameCpuTimings;
    }

    class ScopedRenderFrameCpuTiming final
    {
    public:
        explicit ScopedRenderFrameCpuTiming(RenderFrameCpuPhase phase) noexcept
            : mPhase(phase), bEnabled(sRenderFrameCpuTimingEnabled)
        {
            if (bEnabled) mStart = std::chrono::steady_clock::now();
        }
        ~ScopedRenderFrameCpuTiming()
        {
            if (bEnabled)
                sRenderFrameCpuTimings.Milliseconds[static_cast<size_t>(mPhase)] +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mStart).count();
        }
        ScopedRenderFrameCpuTiming(const ScopedRenderFrameCpuTiming&) = delete;
        ScopedRenderFrameCpuTiming& operator=(const ScopedRenderFrameCpuTiming&) = delete;
    private:
        RenderFrameCpuPhase mPhase;
        bool bEnabled;
        std::chrono::steady_clock::time_point mStart;
    };
}
