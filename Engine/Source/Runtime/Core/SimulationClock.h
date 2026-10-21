#pragma once

#include "Runtime/Core/Timestep.h"

#include <chrono>
#include <cstdint>

namespace HEngine
{
    // Per-world fixed tick accumulator. The presentation clock remains frame based.
    class FixedSimulationStepper final
    {
    public:
        explicit FixedSimulationStepper(double tickHz = 60.0, uint32_t maximumTicks = 4);
        [[nodiscard]] uint32_t Advance(double frameSeconds, bool bPaused = false) noexcept;
        void Reset() noexcept;
        [[nodiscard]] float GetTickSeconds() const noexcept { return static_cast<float>(mTickSeconds); }
        [[nodiscard]] double GetInterpolationAlpha() const noexcept { return mAccumulated / mTickSeconds; }
        [[nodiscard]] double GetDroppedSeconds() const noexcept { return mDroppedSeconds; }
        [[nodiscard]] uint64_t GetTickCount() const noexcept { return mTickCount; }

    private:
        double mTickSeconds;
        uint32_t mMaximumTicks;
        double mAccumulated = 0.0;
        double mDroppedSeconds = 0.0;
        uint64_t mTickCount = 0;
    };

	class SimulationClock final
	{
	public:
		[[nodiscard]] static SimulationClock& GetInstance();

		void Reset();
        void DiscardSuspendedTime() { mLastRealTime = Clock::now(); bFirstFrame = true; }
		void SetPaused(bool bPaused) { mPaused = bPaused; }
		[[nodiscard]] Timestep BeginFrame(
			float fixedDeltaSeconds = 0.0f,
			bool bAdvanceSimulation = true);
		void CompleteFrame();

		// The active frame is one-based. Before the first frame this returns zero.
		[[nodiscard]] std::uint64_t GetFrameNumber() const { return mFrameNumber; }
		[[nodiscard]] std::uint64_t GetCompletedFrameCount() const { return mCompletedFrameCount; }
		[[nodiscard]] double GetElapsedSeconds() const { return mElapsedSeconds; }
        void RecordFixedTicks(uint32_t count, double tickSeconds, double droppedSeconds) noexcept;
        [[nodiscard]] double GetFixedElapsedSeconds() const noexcept { return mFixedElapsedSeconds; }
        [[nodiscard]] double GetDroppedSimulationSeconds() const noexcept { return mDroppedSimulationSeconds; }
        [[nodiscard]] uint64_t GetFixedTickCount() const noexcept { return mFixedTickCount; }
		[[nodiscard]] bool IsFrameInProgress() const { return bFrameInProgress; }
		[[nodiscard]] bool IsPaused() const { return mPaused; }

	private:
		using Clock = std::chrono::steady_clock;

		Clock::time_point mLastRealTime = Clock::now();
		std::uint64_t mFrameNumber = 0;
		std::uint64_t mCompletedFrameCount = 0;
		double mElapsedSeconds = 0.0;
        double mFixedElapsedSeconds = 0.0;
        double mDroppedSimulationSeconds = 0.0;
        uint64_t mFixedTickCount = 0;
		bool bFirstFrame = true;
		bool bFrameInProgress = false;
		bool mPaused = false;
	};
}
