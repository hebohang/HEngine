#pragma once

#include "Game/AgentRuntime/AgentProtocol.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RHI/FrameCaptureSource.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace HEngine
{
    class Application;

    enum class AgentCaptureProcessResult
    {
        None,
        Completed,
        Failed
    };

    class AgentCaptureController final
    {
    public:
        [[nodiscard]] bool Start(
            const std::filesystem::path& sessionDirectory,
            std::string sessionId);
        void Stop();

        [[nodiscard]] Json Request(const Json& params, std::uint64_t renderFrame);
        [[nodiscard]] Json RequestSequence(const Json& params, Application& application, std::uint64_t renderFrame);
        [[nodiscard]] AgentCaptureProcessResult ProcessPending(
            Application& application,
            std::uint64_t renderFrame);
        [[nodiscard]] Json BuildStatus() const;
        [[nodiscard]] bool IsSequencePending() const { return mSequence && (mState == State::Pending || mState == State::InFlight); }

    private:
        enum class State
        {
            Idle,
            Pending,
            InFlight,
            Completed,
            Failed
        };

        [[nodiscard]] bool IsCaptureRootIntact() const;
        [[nodiscard]] static bool IsSafeFilename(std::string_view filename);
        [[nodiscard]] static const char* StateName(State state);
        void FailPending(std::string errorCode);

        std::filesystem::path mCaptureRoot;
        std::filesystem::path mCanonicalCaptureRoot;
        std::string mSessionId;
        std::string mTicket;
        std::string mFilename;
        FrameCaptureSource mSource = FrameCaptureSource::Automatic;
        std::string mErrorCode;
        std::uint64_t mSubmittedCaptures = 0;
        std::uint64_t mSettledCaptures = 0;
        std::uint64_t mRequestedRenderFrame = 0;
        std::uint64_t mSettledRenderFrame = 0;
        State mState = State::Idle;
        bool mActive = false;
        bool mSequence = false;
        Json mSequenceFrames = Json::array();
        Json mSequenceHistoryArtifact = nullptr;
        std::filesystem::path mSequenceHistoryPath;
        Json mRecordedIdentity = nullptr;
    };
}
