#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace HEngine
{
    struct EffectTransformCommand
    {
        int32_t Handle = -1;
        float PositionX = 0.0f;
        float PositionY = 0.0f;
        float PositionZ = 0.0f;
        float ForwardX = 0.0f;
        float ForwardY = 0.0f;
        float ForwardZ = 0.0f;
    };

    using EffectTransformDispatcher = bool(*)(
        void* context,
        const EffectTransformCommand& command);

    void DispatchEffectTransformBatch(
        std::span<const EffectTransformCommand> commands,
        void* context,
        EffectTransformDispatcher dispatcher,
        std::vector<uint8_t>& outResults);
}
