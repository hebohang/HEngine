#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace HEngine
{
    // Pure main-view broad phase. False proves the complete world AABB is
    // outside one expanded clip half-space; uncertain inputs stay visible.
    inline bool IsMainViewAabbVisible(const glm::mat4& viewProjection,
        const glm::vec3& center, const glm::vec3& extents, uint32_t width, uint32_t height) noexcept
    {
        if (!width || !height) return true;
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(center[axis]) || !std::isfinite(extents[axis]) || extents[axis] < 0.f) return true;
        const auto row = [&](int index) {
            return glm::dvec4(viewProjection[0][index], viewProjection[1][index],
                viewProjection[2][index], viewProjection[3][index]);
        };
        for (int column = 0; column < 4; ++column)
            for (int index = 0; index < 4; ++index)
                if (!std::isfinite(viewProjection[column][index])) return true;
        const glm::dvec4 w = row(3), x = row(0), y = row(1), z = row(2);
        // Expand lateral boundaries one native pixel. Depth planes retain an
        // arithmetic guard. This matches the renderer's D3D-style [0,w] depth.
        const std::array<glm::dvec4, 6> planes = {
            w * (1.0 + 2.0 / width) + x, w * (1.0 + 2.0 / width) - x,
            w * (1.0 + 2.0 / height) + y, w * (1.0 + 2.0 / height) - y,
            z, w - z };
        for (const auto& plane : planes)
        {
            const glm::dvec3 normal(plane);
            const double support = glm::dot(glm::abs(normal), glm::dvec3(extents));
            const double distance = glm::dot(normal, glm::dvec3(center)) + plane.w;
            const double scale = glm::dot(glm::abs(normal), glm::abs(glm::dvec3(center))) +
                std::abs(plane.w) + support + 1.0;
            if (distance + support < -1e-5 * scale) return false;
        }
        return true;
    }
}
