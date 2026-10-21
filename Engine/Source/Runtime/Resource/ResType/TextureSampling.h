#pragma once

#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <glm/glm.hpp>

namespace HEngine
{
    REFLECTION_TYPE(TextureSampling)
    STRUCT(TextureSampling, Fields)
    {
        REFLECTION_BODY(TextureSampling);
        int texCoord = 0;
        bool useAffine = false;
        glm::vec3 affineRow0{1,0,0};
        glm::vec3 affineRow1{0,1,0};
        glm::vec2 offset{0.0f};
        glm::vec2 scale{1.0f};
        float rotation = 0.0f;
        int wrapS = 10497;
        int wrapT = 10497;
        int minFilter = 9987;
        int magFilter = 9729;
    };

    // Shared raster/RT structured-buffer layout, 48 bytes per texture slot.
    struct TextureSamplingGpu
    {
        glm::vec4 UvRow0{1,0,0,0};
        glm::vec4 UvRow1{0,1,0,0};
        glm::uvec4 Options{0,0,0,261}; // uv, wrapS, wrapT, min=5 | magLinear=256
    };
    static_assert(sizeof(TextureSamplingGpu) == 48);

    inline TextureSamplingGpu PackTextureSampling(const TextureSampling& sampling)
    {
        const auto wrap = [](int mode) -> uint32_t {
            if (mode == 10497) return 0;
            if (mode == 33071) return 1;
            if (mode == 33648) return 2;
            throw std::invalid_argument("texture_sampler: wrap must be repeat, clamp-to-edge or mirrored-repeat.");
        };
        const auto filter = [](int mode) -> uint32_t {
            if (mode == 9728) return 0;
            if (mode == 9729) return 1;
            if (mode >= 9984 && mode <= 9987) return uint32_t(mode - 9984 + 2);
            throw std::invalid_argument("texture_sampler: unsupported minification filter.");
        };
        if (sampling.texCoord < 0 || sampling.texCoord > 1 ||
            !std::isfinite(sampling.rotation) || !std::isfinite(sampling.offset.x) || !std::isfinite(sampling.offset.y) ||
            !std::isfinite(sampling.scale.x) || !std::isfinite(sampling.scale.y) ||
            (sampling.magFilter != 9728 && sampling.magFilter != 9729))
            throw std::invalid_argument("texture_sampling: UV0/UV1, finite transform and nearest/linear magnification required.");
        const float c = std::cos(sampling.rotation), s = std::sin(sampling.rotation);
        TextureSamplingGpu result;
        result.UvRow0 = {c * sampling.scale.x, -s * sampling.scale.y, sampling.offset.x, 0};
        result.UvRow1 = {s * sampling.scale.x, c * sampling.scale.y, sampling.offset.y, 0};
        if (sampling.useAffine) {
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite(sampling.affineRow0[i]) || !std::isfinite(sampling.affineRow1[i]))
                    throw std::invalid_argument("texture_sampling: affine transform must be finite.");
            result.UvRow0 = glm::vec4(sampling.affineRow0, 0); result.UvRow1 = glm::vec4(sampling.affineRow1, 0);
        }
        result.Options = {uint32_t(sampling.texCoord), wrap(sampling.wrapS), wrap(sampling.wrapT),
            filter(sampling.minFilter) | (sampling.magFilter == 9729 ? 256u : 0u)};
        return result;
    }
}
