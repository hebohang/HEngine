#pragma once

#include "Game/AgentRuntime/AgentProtocol.h"
#include "Runtime/Core/Input/KeyCodes.h"

#include <glm/vec2.hpp>
#include <optional>
#include <string>
#include <vector>

namespace HEngine
{
    class AgentInputController final
    {
    public:
        void Start();
        void Stop();
        void PublishPendingSimulationInput();

        [[nodiscard]] Json ListKeys() const;
        [[nodiscard]] Json SetHeldKeys(const Json& params);
        [[nodiscard]] Json Clear(const Json& params);
        [[nodiscard]] Json Click(
            const Json& params,
            std::uint32_t viewportWidth,
            std::uint32_t viewportHeight);
        [[nodiscard]] Json Pointer(const Json& params, uint32_t width, uint32_t height);
        [[nodiscard]] Json BuildStatus() const;

    private:
        std::optional<std::vector<KeyCode>> mPendingKeys;
        std::optional<glm::vec2> mPendingPointer;
        glm::vec2 mPointerStep{0.0f};
        std::uint32_t mPointerStepsRemaining = 0;
        bool mPendingRightHeld = false;
        bool mActive = false;
        std::uint64_t mVirtualClickCount = 0;
        std::vector<std::string> mHeldKeyNames;
    };
}
