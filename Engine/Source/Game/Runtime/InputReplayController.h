#pragma once

#if defined(HE_DEV_AGENT_RUNTIME)

#include "Game/AgentRuntime/AgentInputController.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace HEngine
{
    class InputReplayController final
    {
    public:
        void Start(const std::filesystem::path& path);
        void Advance();
        void Stop();

    private:
        struct Segment
        {
            std::uint64_t Frames = 0;
            Json Held = Json::array();
        };

        AgentInputController mInput;
        std::vector<Segment> mSegments;
        std::size_t mSegmentIndex = 0;
        std::uint64_t mSegmentFrame = 0;
        std::uint64_t mPlayedFrames = 0;
        bool mActive = false;
        bool mCompleted = false;
    };
}

#endif
