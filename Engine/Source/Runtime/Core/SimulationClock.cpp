#include "Hepch.h"

#include "SimulationClock.h"

namespace HEngine
{
	SimulationClock& SimulationClock::GetInstance()
	{
		static SimulationClock instance;
		return instance;
	}

	void SimulationClock::Reset()
	{
		mLastRealTime = Clock::now();
		mFrameNumber = 0;
		mCompletedFrameCount = 0;
		mElapsedSeconds = 0.0;
		bFirstFrame = true;
		bFrameInProgress = false;
		mPaused = false;
	}

	Timestep SimulationClock::BeginFrame(float fixedDeltaSeconds, bool bAdvanceSimulation)
	{
		HE_CORE_ASSERT(!bFrameInProgress, "Cannot begin a simulation frame before completing the active frame.");

		const Clock::time_point now = Clock::now();
		float deltaSeconds = 0.0f;
		if (!bAdvanceSimulation || mPaused)
		{
			deltaSeconds = 0.0f;
		}
		else if (fixedDeltaSeconds > 0.0f)
		{
			deltaSeconds = fixedDeltaSeconds;
		}
		else if (!bFirstFrame)
		{
			deltaSeconds = std::chrono::duration<float>(now - mLastRealTime).count();
		}

		mLastRealTime = now;
		bFirstFrame = false;
		bFrameInProgress = true;
		mFrameNumber = mCompletedFrameCount + 1;
		mElapsedSeconds += static_cast<double>(deltaSeconds);
		return Timestep(deltaSeconds);
	}

	void SimulationClock::CompleteFrame()
	{
		HE_CORE_ASSERT(bFrameInProgress, "Cannot complete a simulation frame when no frame is active.");

		mCompletedFrameCount = mFrameNumber;
		bFrameInProgress = false;
	}
}
