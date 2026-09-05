#pragma once

#include "Game/AgentRuntime/AgentProtocol.h"
#include "Runtime/Core/Input/KeyCodes.h"

#include <string>
#include <vector>

namespace HEngine
{
    class AgentInputController final
    {
    public:
        void Start();
        void Stop();

        [[nodiscard]] Json ListKeys() const;
        [[nodiscard]] Json SetHeldKeys(const Json& params);
        [[nodiscard]] Json Clear(const Json& params);
        [[nodiscard]] Json Click(
            const Json& params,
            std::uint32_t viewportWidth,
            std::uint32_t viewportHeight);
        [[nodiscard]] Json BuildStatus() const;

    private:
        bool mActive = false;
        std::uint64_t mVirtualClickCount = 0;
        std::vector<std::string> mHeldKeyNames;
    };
}
