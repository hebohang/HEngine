#pragma once

#include "Runtime/Core/Timestep.h"

#include <chrono>
#include <cstdint>

namespace HEngine
{
	class SimulationClock final
	{
	public:
		[[nodiscard]] static SimulationClock& GetInstance();

		void Reset();
		void SetPaused(bool bPaused) { mPaused = bPaused; }
		[[nodiscard]] Timestep BeginFrame(
			float fixedDeltaSeconds = 0.0f,
			bool bAdvanceSimulation = true);
		void CompleteFrame();

		// The active frame is one-based. Before the first frame this returns zero.
		[[nodiscard]] std::uint64_t GetFrameNumber() const { return mFrameNumber; }
		[[nodiscard]] std::uint64_t GetCompletedFrameCount() const { return mCompletedFrameCount; }
		[[nodiscard]] double GetElapsedSeconds() const { return mElapsedSeconds; }
		[[nodiscard]] bool IsFrameInProgress() const { return bFrameInProgress; }
		[[nodiscard]] bool IsPaused() const { return mPaused; }

	private:
		using Clock = std::chrono::steady_clock;

		Clock::time_point mLastRealTime = Clock::now();
		std::uint64_t mFrameNumber = 0;
		std::uint64_t mCompletedFrameCount = 0;
		double mElapsedSeconds = 0.0;
		bool bFirstFrame = true;
		bool bFrameInProgress = false;
		bool mPaused = false;
	};
}
