#pragma once

#include <glm/glm.hpp>

namespace HEngine
{
    // Infinite environment motion depends on camera rotation and raster jitter,
    // never translation. Only a submitted frame may become the previous view.
    class SubmittedSkyMotionHistory
    {
    public:
        glm::mat4 Stage(const glm::mat4& view, const glm::mat4& projection,
            const glm::vec2& jitterPixels, const glm::vec2& renderSize, bool reset)
        {
            glm::mat4 jitter(1.0f);
            jitter[3][0] = 2.0f * jitterPixels.x / renderSize.x;
            jitter[3][1] = -2.0f * jitterPixels.y / renderSize.y;
            mPending = jitter * projection * glm::mat4(glm::mat3(view));
            bPending = true;
            return reset || !bSubmitted ? glm::mat4(1.0f) : mPrevious * glm::inverse(mPending);
        }

        void Commit() noexcept
        {
            if (bPending) { mPrevious = mPending; bSubmitted = true; bPending = false; }
        }
        void Reset() noexcept { bPending = false; bSubmitted = false; }

    private:
        glm::mat4 mPrevious{1.0f}, mPending{1.0f};
        bool bSubmitted = false, bPending = false;
    };
}
