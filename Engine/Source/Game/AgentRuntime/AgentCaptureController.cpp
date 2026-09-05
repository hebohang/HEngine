#include "Game/AgentRuntime/AgentCaptureController.h"

#include "Game/AgentRuntime/AgentCommandRegistry.h"
#include "Runtime/Core/AppFramework/Application.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace HEngine
{
    namespace
    {
        constexpr std::size_t MaxCaptureFilenameBytes = 64;

        [[nodiscard]] std::string UpperAscii(std::string_view text)
        {
            std::string result(text);
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::toupper(character));
            });
            return result;
        }

        [[nodiscard]] bool IsWindowsReservedStem(std::string_view filename)
        {
            const std::size_t firstDot = filename.find('.');
            const std::string stem = UpperAscii(filename.substr(0, firstDot));
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
                return true;
            if (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")))
                return stem[3] >= '1' && stem[3] <= '9';
            return false;
        }
    }

    bool AgentCaptureController::Start(
        const std::filesystem::path& sessionDirectory,
        std::string sessionId)
    {
        Stop();
        if (sessionDirectory.empty() || !sessionDirectory.is_absolute() || sessionId.empty())
            return false;

        mCaptureRoot = (sessionDirectory / "captures").lexically_normal();
        std::error_code error;
        std::filesystem::create_directory(mCaptureRoot, error);
        if (error && !std::filesystem::is_directory(mCaptureRoot, error))
            return false;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(mCaptureRoot, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
            return false;
        mCanonicalCaptureRoot = std::filesystem::canonical(mCaptureRoot, error);
        if (error || mCanonicalCaptureRoot.empty())
            return false;

        mSessionId = std::move(sessionId);
        mTicket.clear();
        mFilename.clear();
        mErrorCode.clear();
        mSubmittedCaptures = 0;
        mSettledCaptures = 0;
        mRequestedRenderFrame = 0;
        mSettledRenderFrame = 0;
        mState = State::Idle;
        mActive = true;
        return true;
    }

    void AgentCaptureController::Stop()
    {
        if (mState == State::Pending || mState == State::InFlight)
            FailPending("session_stopped");
        mActive = false;
    }

    Json AgentCaptureController::Request(const Json& params, std::uint64_t renderFrame)
    {
        for (const auto& [name, value] : params.items())
        {
            (void)value;
            if (name != "filename")
                throw std::invalid_argument("capture.frame contains an unknown parameter.");
        }
        if (!mActive)
            throw AgentCommandError("capture_unavailable", "Frame capture is not active.");
        if (mState == State::Pending || mState == State::InFlight)
            throw AgentCommandError("capture_pending", "A frame capture ticket is already pending.");
        if (!params.contains("filename") || !params["filename"].is_string())
            throw std::invalid_argument("capture.frame filename is required.");

        const std::string filename = params["filename"].get<std::string>();
        if (!IsSafeFilename(filename))
        {
            throw std::invalid_argument(
                "capture.frame filename must be a safe ASCII .png name of at most 64 bytes.");
        }
        if (!IsCaptureRootIntact())
            throw AgentCommandError("capture_root_invalid", "The capture output directory is unavailable.");

        std::error_code error;
        const std::filesystem::path outputPath = mCaptureRoot / filename;
        const std::filesystem::file_status outputStatus =
            std::filesystem::symlink_status(outputPath, error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw AgentCommandError("capture_output_check_failed", "The capture output could not be checked.");
        if (!error && std::filesystem::exists(outputStatus))
            throw AgentCommandError("capture_output_exists", "The requested capture filename already exists.");

        ++mSubmittedCaptures;
        mTicket = mSessionId + "-capture-" + std::to_string(mSubmittedCaptures);
        mFilename = filename;
        mErrorCode.clear();
        mRequestedRenderFrame = renderFrame;
        mSettledRenderFrame = 0;
        mState = State::Pending;

        Json result = BuildStatus();
        result["ticket"] = mTicket;
        result["targetSettledCaptures"] = mSubmittedCaptures;
        return result;
    }

    AgentCaptureProcessResult AgentCaptureController::ProcessPending(
        Application& application,
        std::uint64_t renderFrame)
    {
        if (!mActive || (mState != State::Pending && mState != State::InFlight))
            return AgentCaptureProcessResult::None;

        if (mState == State::Pending && !IsCaptureRootIntact())
        {
            FailPending("capture_root_invalid");
            mSettledRenderFrame = renderFrame;
            return AgentCaptureProcessResult::Failed;
        }

        if (mState == State::Pending)
        {
            const std::filesystem::path outputPath = mCaptureRoot / mFilename;
            std::error_code error;
            const std::filesystem::file_status outputStatus =
                std::filesystem::symlink_status(outputPath, error);
            if ((error && error != std::errc::no_such_file_or_directory) ||
                (!error && std::filesystem::exists(outputStatus)))
            {
                FailPending("capture_output_exists");
                mSettledRenderFrame = renderFrame;
                return AgentCaptureProcessResult::Failed;
            }

            if (!application.RequestFrameCapture(outputPath))
            {
                FailPending("capture_busy");
                mSettledRenderFrame = renderFrame;
                return AgentCaptureProcessResult::Failed;
            }
            mState = State::InFlight;
            return AgentCaptureProcessResult::None;
        }

        const RuntimeFrameCaptureProgress& progress = application.GetFrameCaptureProgress();
        if (progress.State != RuntimeFrameCaptureState::Complete &&
            progress.State != RuntimeFrameCaptureState::Failed)
        {
            return AgentCaptureProcessResult::None;
        }

        mSettledRenderFrame = renderFrame;
        ++mSettledCaptures;
        const bool bCompleted = progress.State == RuntimeFrameCaptureState::Complete;
        mState = bCompleted ? State::Completed : State::Failed;
        mErrorCode = bCompleted ? std::string{} : "capture_failed";
        application.ClearFrameCapture();
        return bCompleted
            ? AgentCaptureProcessResult::Completed
            : AgentCaptureProcessResult::Failed;
    }

    Json AgentCaptureController::BuildStatus() const
    {
        Json latest = nullptr;
        if (mState != State::Idle)
        {
            latest = Json{
                { "ticket", mTicket },
                { "filename", mFilename },
                { "relativePath", std::string("captures/") + mFilename },
                { "state", StateName(mState) },
                { "requestedRenderFrame", mRequestedRenderFrame }
            };
            if (mState == State::Completed || mState == State::Failed)
                latest["settledRenderFrame"] = mSettledRenderFrame;
            if (mState == State::Failed)
                latest["error"] = mErrorCode;
        }
        return Json{
            { "active", mActive },
            { "outputDirectory", "captures" },
            { "submittedCaptures", mSubmittedCaptures },
            { "settledCaptures", mSettledCaptures },
            { "pending", mState == State::Pending || mState == State::InFlight },
            { "latest", std::move(latest) }
        };
    }

    bool AgentCaptureController::IsCaptureRootIntact() const
    {
        if (mCaptureRoot.empty() || mCanonicalCaptureRoot.empty())
            return false;
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(mCaptureRoot, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
            return false;
        const std::filesystem::path currentCanonical =
            std::filesystem::canonical(mCaptureRoot, error);
        return !error && currentCanonical == mCanonicalCaptureRoot;
    }

    bool AgentCaptureController::IsSafeFilename(std::string_view filename)
    {
        if (filename.size() < 5 || filename.size() > MaxCaptureFilenameBytes ||
            !filename.ends_with(".png") || filename.find("..") != std::string_view::npos ||
            IsWindowsReservedStem(filename))
        {
            return false;
        }
        if (std::isalnum(static_cast<unsigned char>(filename.front())) == 0 &&
            filename.front() != '_')
        {
            return false;
        }
        return std::all_of(filename.begin(), filename.end(), [](unsigned char character)
        {
            return character <= 0x7Fu && (std::isalnum(character) != 0 || character == '_' ||
                character == '-' || character == '.');
        });
    }

    const char* AgentCaptureController::StateName(State state)
    {
        switch (state)
        {
        case State::Idle: return "idle";
        case State::Pending: return "pending";
        case State::InFlight: return "in_flight";
        case State::Completed: return "completed";
        case State::Failed: return "failed";
        }
        return "unknown";
    }

    void AgentCaptureController::FailPending(std::string errorCode)
    {
        if (mState == State::Pending || mState == State::InFlight)
            ++mSettledCaptures;
        mState = State::Failed;
        mErrorCode = std::move(errorCode);
    }
}
