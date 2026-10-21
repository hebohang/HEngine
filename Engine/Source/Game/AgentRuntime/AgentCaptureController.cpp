#include "Game/AgentRuntime/AgentCaptureController.h"

#include "Game/AgentRuntime/AgentCommandRegistry.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"

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
        mSource = FrameCaptureSource::Automatic;
        mErrorCode.clear();
        mSubmittedCaptures = 0;
        mSettledCaptures = 0;
        mRequestedRenderFrame = 0;
        mSettledRenderFrame = 0;
        mState = State::Idle;
        mSequence = false;
        mSequenceFrames = Json::array();
        mActive = true;
        return true;
    }

    void AgentCaptureController::Stop()
    {
        if (mSequence && (mState == State::Pending || mState == State::InFlight))
            Application::GetInstance().CancelFrameCaptureSequence();
        if (mState == State::Pending || mState == State::InFlight)
            FailPending("session_stopped");
        mActive = false;
    }

    Json AgentCaptureController::Request(const Json& params, std::uint64_t renderFrame)
    {
        for (const auto& [name, value] : params.items())
        {
            (void)value;
            if (name != "filename" && name != "delayFrames" && name != "source")
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
                "capture.frame filename must be a safe ASCII .png/.pfm/.raw name of at most 64 bytes.");
        }
        FrameCaptureSource source = FrameCaptureSource::Automatic;
        const auto sourceValue = params.find("source");
        if (sourceValue != params.end())
        {
            if (!sourceValue->is_string()) throw std::invalid_argument("Capture source must be a string");
            const auto name = sourceValue->get<std::string>();
            if (name == "automatic") source = FrameCaptureSource::Automatic;
            else if (name == "depth") source = FrameCaptureSource::Depth;
            else if (name == "motion") source = FrameCaptureSource::Motion;
            else if (name == "normal") source = FrameCaptureSource::Normal;
            else if (name == "surface-id") source = FrameCaptureSource::SurfaceId;
            else if (name == "reactive") source = FrameCaptureSource::Reactive;
            else if (name == "composition") source = FrameCaptureSource::Composition;
            else if (name == "upscale-motion") source = FrameCaptureSource::UpscaleMotion;
            else if (name == "upscale-color") source = FrameCaptureSource::UpscaleColor;
            else if (name == "upscale-opaque") source = FrameCaptureSource::UpscaleOpaque;
            else throw std::invalid_argument("Unknown capture source: " + name);
        }
        if (source != FrameCaptureSource::Automatic && !filename.ends_with(".raw"))
            throw std::invalid_argument("Explicit native capture sources require .raw output");
        if (!Application::GetInstance().IsFrameCaptureSourceAvailable(source))
            throw AgentCommandError("capture_source_unavailable", FrameCaptureSourceName(source));
        const auto delay = params.find("delayFrames");
        if (delay != params.end() &&
            (!delay->is_number_integer() || *delay < 0 || *delay > 600))
            throw std::invalid_argument("Capture delayFrames must be 0..600 rendered frames.");
        const int delayFrames = delay != params.end() ? delay->get<int>() : 0;
        if (!IsCaptureRootIntact())
            throw AgentCommandError("capture_root_invalid", "The capture output directory is unavailable.");

        const std::filesystem::path outputPath = mCaptureRoot / filename;
        for (const char* suffix : { "", ".json", ".world.json" })
        {
            auto artifactPath = outputPath;
            artifactPath += suffix;
            std::error_code error;
            const auto outputStatus = std::filesystem::symlink_status(artifactPath, error);
            if (error && error != std::errc::no_such_file_or_directory)
                throw AgentCommandError("capture_output_check_failed", "The capture output could not be checked.");
            if (!error && std::filesystem::exists(outputStatus))
                throw AgentCommandError("capture_output_exists", "A requested capture artifact already exists.");
        }

        ++mSubmittedCaptures;
        mTicket = mSessionId + "-capture-" + std::to_string(mSubmittedCaptures);
        mFilename = filename;
        mSource = source;
        mSequence = false;
        mSequenceFrames = Json::array();
        mRecordedIdentity = nullptr;
        mErrorCode.clear();
        mRequestedRenderFrame = renderFrame + static_cast<std::uint64_t>(delayFrames);
        mSettledRenderFrame = 0;
        mState = State::Pending;

        Json result = BuildStatus();
        result["ticket"] = mTicket;
        result["targetSettledCaptures"] = mSubmittedCaptures;
        return result;
    }

    Json AgentCaptureController::RequestSequence(const Json& params, Application& application, std::uint64_t renderFrame)
    {
        for (const auto& [key, value] : params.items())
            if (key != "prefix" && key != "format" && key != "source" && key != "frames" && key != "cameraCut" && key != "settleFrames") throw std::invalid_argument("Unknown capture.sequence parameter");
        if (!params.contains("prefix") || !params["prefix"].is_string() ||
            !params.contains("frames") || !params["frames"].is_number_integer() ||
            params["frames"] < 8 || params["frames"] > 16)
            throw std::invalid_argument("capture.sequence requires prefix and frames in 8..16");
        if (params.contains("cameraCut") && !params["cameraCut"].is_boolean())
            throw std::invalid_argument("capture.sequence cameraCut must be boolean");
        const bool bCameraCut = params.value("cameraCut", false);
        if (params.contains("settleFrames") && (!bCameraCut || !params["settleFrames"].is_number_integer() ||
            params["settleFrames"] < 0 || params["settleFrames"] > 600))
            throw std::invalid_argument("settleFrames requires cameraCut=true and integer 0..600");
        const uint32_t settleFrames = params.value("settleFrames", uint32_t(0));
        if (params.contains("format") && !params["format"].is_string())
            throw std::invalid_argument("capture.sequence format must be png, pfm or raw");
        const std::string format = params.value("format", std::string("png"));
        if (format != "png" && format != "pfm" && format != "raw")
            throw std::invalid_argument("capture.sequence format must be png, pfm or raw");
        const auto prefix = params["prefix"].get<std::string>();
        const auto count = params["frames"].get<size_t>();
        std::vector<std::filesystem::path> paths;
        for (size_t index = 0; index < count; ++index)
        {
            const auto filename = prefix + "-" + std::to_string(index) + "." + format;
            if (!IsSafeFilename(filename)) throw std::invalid_argument("Unsafe capture sequence prefix");
            const auto path = mCaptureRoot / filename;
            for (const char* suffix : { "", ".json", ".world.json" })
            {
                auto artifact = path; artifact += suffix;
                std::error_code error;
                const auto status = std::filesystem::symlink_status(artifact, error);
                if ((error && error != std::errc::no_such_file_or_directory) ||
                    (!error && std::filesystem::exists(status)))
                    throw AgentCommandError("capture_output_exists", "Sequence output already exists or cannot be checked");
            }
            paths.push_back(path);
        }
        std::filesystem::path historyPath;
        if (bCameraCut)
        {
            historyPath = paths.front(); historyPath += ".sequence-history.json";
            std::error_code error;
            const auto status = std::filesystem::symlink_status(historyPath, error);
            if ((error && error != std::errc::no_such_file_or_directory) ||
                (!error && std::filesystem::exists(status)))
                throw AgentCommandError("capture_output_exists", "Sequence history output exists or cannot be checked");
        }
        // Reuse the single-frame source and encoding validation before reserving
        // any sequence work or allowing the caller to reset camera history.
        Json request{{"filename", paths.front().filename().string()}};
        if (params.contains("source")) request["source"] = params.at("source");
        (void)Request(request, renderFrame);
        if (!application.RequestFrameCaptureSequence(std::move(paths), bCameraCut, settleFrames, mSource))
        {
            FailPending("capture_busy");
            throw AgentCommandError("capture_busy", "Application capture is busy");
        }
        mSequenceHistoryArtifact = nullptr;
        mSequenceHistoryPath = std::move(historyPath);
        mSequence = true;
        mState = State::InFlight;
        auto result = BuildStatus();
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
        if (mSequence && !IsCaptureRootIntact())
            application.CancelFrameCaptureSequence();
        if (mState == State::Pending && renderFrame < mRequestedRenderFrame)
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
            for (const char* suffix : { "", ".json", ".world.json" })
            {
                auto artifactPath = outputPath;
                artifactPath += suffix;
                std::error_code error;
                const auto outputStatus = std::filesystem::symlink_status(artifactPath, error);
                if ((error && error != std::errc::no_such_file_or_directory) ||
                    (!error && std::filesystem::exists(outputStatus)))
                {
                    FailPending("capture_output_exists");
                    mSettledRenderFrame = renderFrame;
                    return AgentCaptureProcessResult::Failed;
                }
            }

            bool accepted = false;
            try { accepted = application.RequestFrameCapture(outputPath, mSource); }
            catch (const CaptureSourceUnavailable&)
            {
                FailPending("capture_source_unavailable");
                mSettledRenderFrame = renderFrame;
                return AgentCaptureProcessResult::Failed;
            }
            if (!accepted)
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
        mRecordedIdentity = Json{ {"valid", progress.Submission != 0},
            {"applicationSimulationFrame", progress.RenderFrame},
            {"renderWorldFrame", progress.RenderWorldFrame}, {"submission", progress.Submission} };
        if (mSequence && progress.State == RuntimeFrameCaptureState::Failed)
            application.CancelFrameCaptureSequence();
        bool bHistorySaved = true;
        if (mSequence)
        {
            if (!mSequenceHistoryPath.empty())
            {
                try
                {
                    if (!IsCaptureRootIntact()) throw std::runtime_error("Capture root changed before history publication");
                    const auto& records = application.GetCaptureSequenceHistory();
                    if (records.size() > 616) throw std::runtime_error("Sequence history exceeded its bound");
                    const std::string bytes = Json{{"schema", "he.atomic-sequence-history.v1"},
                        {"ticket", mTicket}, {"records", records}}.dump();
                    if (bytes.size() > 4 * 1024 * 1024) throw std::runtime_error("Sequence history artifact exceeded its bound");
                    Utils::WriteTextToFileAtomically(mSequenceHistoryPath, bytes, Utils::AtomicWriteMode::CreateNew);
                    mSequenceHistoryArtifact = Json{{"schema", "he.atomic-sequence-history.v1"},
                        {"relativePath", "captures/" + mSequenceHistoryPath.filename().string()},
                        {"recordCount", records.size()}, {"bytes", bytes.size()}};
                }
                catch (...)
                {
                    bHistorySaved = false;
                    application.CancelFrameCaptureSequence();
                }
            }
            mSequenceFrames = Json::array();
            for (const auto& frame : application.GetFrameCaptureSequence())
                mSequenceFrames.push_back({ {"relativePath", "captures/" + frame.OutputPath.filename().string()},
                    {"renderFrame", frame.RenderFrame}, {"submission", frame.Submission},
                    {"recordedIdentity", Json{{"valid", frame.Submission != 0},
                        {"applicationSimulationFrame", frame.RenderFrame},
                        {"renderWorldFrame", frame.RenderWorldFrame}, {"submission", frame.Submission}}},
                    {"simulationTime", frame.SimulationTime}, {"simulationStep", frame.SimulationStep},
                    {"state", frame.State == RuntimeFrameCaptureState::Complete ? "completed" : "failed"}, {"error", frame.Error} });
        }
        ++mSettledCaptures;
        const bool bCompleted = progress.State == RuntimeFrameCaptureState::Complete && bHistorySaved;
        mState = bCompleted ? State::Completed : State::Failed;
        mErrorCode = !bHistorySaved ? "sequence_history_publication_failed" :
            bCompleted ? std::string{} : (progress.Error.empty() ? "capture_failed" : progress.Error);
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
                { "source", FrameCaptureSourceName(mSource) },
                { "relativePath", std::string("captures/") + mFilename },
                { "state", StateName(mState) },
                { "requestedRenderFrame", mRequestedRenderFrame }
            };
            if (mState == State::Completed || mState == State::Failed)
                latest["settledRenderFrame"] = mSettledRenderFrame;
            if (mState == State::Failed)
                latest["error"] = mErrorCode;
        }
        if (!latest.is_null()) latest["recordedIdentity"] = mRecordedIdentity;
        if (mSequence)
        {
            latest["frames"] = mSequenceFrames;
            latest["sequenceHistoryArtifact"] = mSequenceHistoryArtifact;
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
            (!filename.ends_with(".png") && !filename.ends_with(".pfm") && !filename.ends_with(".raw")) ||
            filename.find("..") != std::string_view::npos ||
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
