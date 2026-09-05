#include "Hepch.h"

#include "PresentationClock.h"

#include <algorithm>
#include <cmath>

namespace HEngine
{
	PresentationClock& PresentationClock::GetInstance()
	{
		static PresentationClock instance;
		return instance;
	}

	void PresentationClock::Reset()
	{
		mLastRealTime = Clock::now();
		mFrameNumber = 0;
		mCompletedFrameCount = 0;
		mElapsedSeconds = 0.0;
		mDeltaSeconds = 0.0f;
		bFirstFrame = true;
		bFrameInProgress = false;
	}

	Timestep PresentationClock::BeginFrame(float fixedDeltaSeconds, bool bAdvancePresentation)
	{
		HE_CORE_ASSERT(!bFrameInProgress, "Cannot begin a presentation frame before completing the active frame.");

		const Clock::time_point now = Clock::now();
		float deltaSeconds = 0.0f;
		if (bAdvancePresentation)
		{
			if (fixedDeltaSeconds > 0.0f && std::isfinite(fixedDeltaSeconds))
				deltaSeconds = fixedDeltaSeconds;
			else if (!bFirstFrame)
				deltaSeconds = std::chrono::duration<float>(now - mLastRealTime).count();

			deltaSeconds = std::clamp(deltaSeconds, 0.0f, GetMaximumDeltaSeconds());
		}

		// Always rebase the real-time origin. Frames spent suspended or minimized
		// must not accumulate into the first visible frame after resume.
		mLastRealTime = now;
		bFirstFrame = false;
		bFrameInProgress = true;
		mFrameNumber = mCompletedFrameCount + 1;
		mDeltaSeconds = deltaSeconds;
		mElapsedSeconds += static_cast<double>(deltaSeconds);
		return Timestep(deltaSeconds);
	}

	void PresentationClock::CompleteFrame()
	{
		HE_CORE_ASSERT(bFrameInProgress, "Cannot complete a presentation frame when no frame is active.");

		mCompletedFrameCount = mFrameNumber;
		bFrameInProgress = false;
	}
}
