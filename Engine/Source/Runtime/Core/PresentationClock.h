#pragma once

#include "Runtime/Core/Timestep.h"

#include <chrono>
#include <cstdint>

namespace HEngine
{
	// Frame clock for presentation-only systems. It is deliberately independent
	// from gameplay pause and time scale, and can be fixed-step for deterministic
	// captures and tests.
	class PresentationClock final
	{
	public:
		[[nodiscard]] static PresentationClock& GetInstance();

		void Reset();
		[[nodiscard]] Timestep BeginFrame(
			float fixedDeltaSeconds = 0.0f,
			bool bAdvancePresentation = true);
		void CompleteFrame();

		[[nodiscard]] std::uint64_t GetFrameNumber() const { return mFrameNumber; }
		[[nodiscard]] std::uint64_t GetCompletedFrameCount() const { return mCompletedFrameCount; }
		[[nodiscard]] double GetElapsedSeconds() const { return mElapsedSeconds; }
		[[nodiscard]] float GetDeltaSeconds() const { return mDeltaSeconds; }
		[[nodiscard]] bool IsFrameInProgress() const { return bFrameInProgress; }

		static constexpr float GetMaximumDeltaSeconds() { return 0.1f; }

	private:
		using Clock = std::chrono::steady_clock;

		Clock::time_point mLastRealTime = Clock::now();
		std::uint64_t mFrameNumber = 0;
		std::uint64_t mCompletedFrameCount = 0;
		double mElapsedSeconds = 0.0;
		float mDeltaSeconds = 0.0f;
		bool bFirstFrame = true;
		bool bFrameInProgress = false;
	};
}
