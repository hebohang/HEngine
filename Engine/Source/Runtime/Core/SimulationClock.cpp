#include "Hepch.h"

#include "SimulationClock.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace HEngine
{
    FixedSimulationStepper::FixedSimulationStepper(double tickHz, uint32_t maximumTicks)
        : mTickSeconds(1.0 / tickHz), mMaximumTicks(maximumTicks)
    {
        if (!std::isfinite(tickHz) || tickHz <= 0.0 || maximumTicks == 0)
            throw std::invalid_argument("Fixed simulation tick configuration is invalid");
    }

    uint32_t FixedSimulationStepper::Advance(double frameSeconds, bool bPaused) noexcept
    {
        if (bPaused)
        {
            // Preserve the fractional presentation phase. Dropping it makes an
            // interpolated character jump backwards when pausing or resuming.
            // Paused wall time is never added to the accumulator.
            return 0;
        }
        if (!std::isfinite(frameSeconds) || frameSeconds <= 0.0) return 0;
        mAccumulated += frameSeconds;
        const double maximumAccumulated = mTickSeconds * mMaximumTicks;
        if (mAccumulated > maximumAccumulated)
        {
            mDroppedSeconds += mAccumulated - maximumAccumulated;
            mAccumulated = maximumAccumulated;
        }
        const uint32_t ticks = std::min(mMaximumTicks,
            static_cast<uint32_t>((mAccumulated + 1e-9) / mTickSeconds));
        mAccumulated -= ticks * mTickSeconds;
        if (mAccumulated < 0.0) mAccumulated = 0.0;
        mTickCount += ticks;
        return ticks;
    }

    void FixedSimulationStepper::Reset() noexcept
    {
        mAccumulated = 0.0;
        mDroppedSeconds = 0.0;
        mTickCount = 0;
    }

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
        mFixedElapsedSeconds = 0.0;
        mDroppedSimulationSeconds = 0.0;
        mFixedTickCount = 0;
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

    void SimulationClock::RecordFixedTicks(
        uint32_t count, double tickSeconds, double droppedSeconds) noexcept
    {
        mFixedTickCount += count;
        mFixedElapsedSeconds += count * tickSeconds;
        mDroppedSimulationSeconds += droppedSeconds;
    }
}
