#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace HEngine
{
    // Presentation-only interpolation; collision and gameplay keep the current
    // fixed-tick pose. Reset on teleports rather than interpolating through walls.
    class FixedPositionHistory final
    {
    public:
        void Reset(const glm::vec3& position) noexcept
        { mPrevious = mCurrent = position; bValid = true; }

        void Advance(const glm::vec3& previous, const glm::vec3& current) noexcept
        { mPrevious = previous; mCurrent = current; bValid = true; }

        [[nodiscard]] glm::vec3 Offset(const glm::vec3& authoritative, double alpha) const noexcept
        {
            const glm::vec3 discrepancy = authoritative - mCurrent;
            if (!bValid || !std::isfinite(alpha) || glm::dot(discrepancy, discrepancy) > 1e-6f)
                return glm::vec3(0.0f);
            return glm::mix(mPrevious, mCurrent, static_cast<float>(std::clamp(alpha, 0.0, 1.0))) - mCurrent;
        }

    private:
        glm::vec3 mPrevious{0.0f};
        glm::vec3 mCurrent{0.0f};
        bool bValid = false;
    };
}
