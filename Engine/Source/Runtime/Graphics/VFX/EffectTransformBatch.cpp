#include "Hepch.h"

#include "Runtime/Graphics/VFX/EffectTransformBatch.h"

#include <cstddef>

namespace HEngine
{
    void DispatchEffectTransformBatch(
        std::span<const EffectTransformCommand> commands,
        void* context,
        EffectTransformDispatcher dispatcher,
        std::vector<uint8_t>& outResults)
    {
        outResults.assign(commands.size(), uint8_t{ 0 });
        if (dispatcher == nullptr)
            return;

        for (std::size_t index = 0; index < commands.size(); ++index)
        {
            outResults[index] = dispatcher(context, commands[index])
                ? uint8_t{ 1 }
                : uint8_t{ 0 };
        }
    }
}
