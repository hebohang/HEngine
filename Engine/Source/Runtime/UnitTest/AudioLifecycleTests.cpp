#include "Hepch.h"

#include "Runtime/UnitTest/AudioLifecycleTests.h"

#include "Runtime/Audio/AudioBackend.h"
#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Audio/ProjectAudioPath.h"
#include "Runtime/Audio/ScriptAudio.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        class ScopedTempDirectory final
        {
        public:
            ScopedTempDirectory()
            {
                static std::atomic_uint64_t sSuffix = 0;
                const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                mPath = std::filesystem::temp_directory_path() /
                    ("HEngine_AudioLifecycle_" + std::to_string(timestamp) + "_" +
                        std::to_string(sSuffix.fetch_add(1, std::memory_order_relaxed)));
                std::filesystem::create_directories(mPath);
            }

            ~ScopedTempDirectory()
            {
                std::error_code error;
                std::filesystem::remove_all(mPath, error);
            }

            [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
            {
                return mPath;
            }

        private:
            std::filesystem::path mPath;
        };

        void WriteFixture(const std::filesystem::path& path)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            Require(static_cast<bool>(file), "Could not create an audio path fixture.");
            file.put('\0');
            Require(static_cast<bool>(file), "Could not write an audio path fixture.");
        }

        [[nodiscard]] bool IsExpectedSymlinkCreationFailure(
            const std::error_code& error) noexcept
        {
            return error == std::errc::permission_denied ||
                error == std::errc::operation_not_permitted ||
                error == std::errc::function_not_supported;
        }

        struct FakeAudioBackendState
        {
            std::filesystem::path LastPath;
            AudioPlayParams LastParams;
            glm::vec3 LastVelocity{ 0.0f };
            AudioHandle LastStoppedHandle{};
            AudioListener LastListener;
            std::array<float, static_cast<std::size_t>(AudioBus::Count)> BusVolumes{
                1.0f, 1.0f, 1.0f
            };
            std::string LastError;
            int InitializeCalls = 0;
            int ShutdownCalls = 0;
            int UpdateCalls = 0;
            int RemainingInitializeFailures = 1;
            float MasterVolume = 1.0f;
            bool bInitialized = false;
            bool bPaused = false;
            bool bGameplayPaused = false;
        };

        class FakeAudioBackend final : public AudioBackend
        {
        public:
            explicit FakeAudioBackend(std::shared_ptr<FakeAudioBackendState> state)
                : mState(std::move(state))
            {
            }

            bool Initialize() override
            {
                ++mState->InitializeCalls;
                if (mState->RemainingInitializeFailures > 0)
                {
                    --mState->RemainingInitializeFailures;
                    mState->LastError = "injected initialization failure";
                    return false;
                }
                mState->LastError.clear();
                mState->bInitialized = true;
                return true;
            }

            void Shutdown() override
            {
                ++mState->ShutdownCalls;
                mState->bInitialized = false;
                mState->bPaused = false;
            }

            void Update() override
            {
                if (mState->bInitialized)
                    ++mState->UpdateCalls;
            }

            [[nodiscard]] bool IsInitialized() const override { return mState->bInitialized; }
            [[nodiscard]] bool IsDeviceAvailable() const override { return mState->bInitialized; }
            [[nodiscard]] std::string GetLastError() const override { return mState->LastError; }

            AudioHandle Play(
                const std::filesystem::path& path,
                const AudioPlayParams& params) override
            {
                if (!mState->bInitialized)
                    return {};
                mState->LastPath = path;
                mState->LastParams = params;
                return AudioHandle{ 41 };
            }

            AudioHandle PlayMemory(
                std::shared_ptr<std::vector<uint8_t>> data,
                const std::string&,
                const AudioPlayParams& params) override
            {
                if (!mState->bInitialized || !data || data->empty())
                    return {};
                mState->LastParams = params;
                return AudioHandle{ 42 };
            }

            bool Stop(AudioHandle handle) override
            {
                if (!IsKnownHandle(handle))
                    return false;
                mState->LastStoppedHandle = handle;
                return true;
            }

            bool Pause(AudioHandle handle, bool bPaused) override
            {
                if (!IsKnownHandle(handle))
                    return false;
                mState->bPaused = bPaused;
                return true;
            }

            [[nodiscard]] bool IsPlaying(AudioHandle handle) const override
            {
                return IsKnownHandle(handle) && !mState->bPaused;
            }

            [[nodiscard]] bool IsPaused(AudioHandle handle) const override
            {
                return IsKnownHandle(handle) && mState->bPaused;
            }

            [[nodiscard]] AudioPlaybackState GetState(AudioHandle handle) const override
            {
                if (!IsKnownHandle(handle))
                    return AudioPlaybackState::Invalid;
                return mState->bPaused ? AudioPlaybackState::Paused : AudioPlaybackState::Playing;
            }

            bool SetVolume(AudioHandle handle, float) override { return IsKnownHandle(handle); }
            bool SetPitch(AudioHandle handle, float) override { return IsKnownHandle(handle); }
            bool SetPan(AudioHandle handle, float) override { return IsKnownHandle(handle); }
            bool SetPosition(AudioHandle handle, const glm::vec3&) override { return IsKnownHandle(handle); }
            bool SetDirection(AudioHandle handle, const glm::vec3&) override { return IsKnownHandle(handle); }

            bool SetVelocity(AudioHandle handle, const glm::vec3& velocity) override
            {
                if (!IsKnownHandle(handle))
                    return false;
                mState->LastVelocity = velocity;
                return true;
            }

            bool SetCone(AudioHandle handle, float, float, float) override
            {
                return IsKnownHandle(handle);
            }

            bool Seek(AudioHandle handle, double) override { return IsKnownHandle(handle); }
            [[nodiscard]] double GetPlaybackPosition(AudioHandle handle) const override
            {
                return IsKnownHandle(handle) ? 1.25 : 0.0;
            }
            [[nodiscard]] double GetLength(AudioHandle handle) const override
            {
                return IsKnownHandle(handle) ? 2.5 : 0.0;
            }

            void SetMasterVolume(float volume) override { mState->MasterVolume = volume; }
            [[nodiscard]] float GetMasterVolume() const override { return mState->MasterVolume; }

            void SetBusVolume(AudioBus bus, float volume) override
            {
                const auto index = static_cast<std::size_t>(bus);
                if (index < mState->BusVolumes.size())
                    mState->BusVolumes[index] = volume;
            }

            [[nodiscard]] float GetBusVolume(AudioBus bus) const override
            {
                const auto index = static_cast<std::size_t>(bus);
                return index < mState->BusVolumes.size() ? mState->BusVolumes[index] : 0.0f;
            }

            void StopBus(AudioBus) override {}
            void SetGameplayPaused(bool bPaused) override { mState->bGameplayPaused = bPaused; }
            [[nodiscard]] bool IsGameplayPaused() const override { return mState->bGameplayPaused; }

            bool SetListener(uint32_t index, const AudioListener& listener) override
            {
                if (!mState->bInitialized || index != 0)
                    return false;
                mState->LastListener = listener;
                return true;
            }

        private:
            [[nodiscard]] bool IsKnownHandle(AudioHandle handle) const
            {
                return mState->bInitialized && (handle.Value == 41 || handle.Value == 42);
            }

            std::shared_ptr<FakeAudioBackendState> mState;
        };

        void VerifyStrictProjectAudioPaths()
        {
            ScopedTempDirectory temporaryDirectory;
            const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
            const std::filesystem::path audioFixture = projectRoot / "Assets/Audio/Test.wav";
            WriteFixture(audioFixture);

            ProjectAudioPath resolvedPath;
            Utils::PathValidationResult result = TryResolveProjectAudioPath(
                projectRoot,
                "Assets\\Audio/./Test.wav",
                resolvedPath);
            Require(static_cast<bool>(result), "A valid project audio path was rejected.");
            Require(resolvedPath.NormalizedPath == "Assets/Audio/Test.wav",
                "A project audio path was not normalized.");
            Require(resolvedPath.FullPath == std::filesystem::weakly_canonical(audioFixture),
                "A project audio path resolved to the wrong file.");

            result = TryResolveProjectAudioPath(projectRoot, "../Outside.wav", resolvedPath);
            Require(!result && result.Code == Utils::PathValidationErrorCode::ParentTraversal,
                "A parent-traversing audio path was accepted.");
            Require(resolvedPath.FullPath.empty() && resolvedPath.NormalizedPath.empty(),
                "A failed audio path resolution retained output state.");

            result = TryResolveProjectAudioPath(projectRoot, "C:\\Outside.wav", resolvedPath);
            Require(!result && result.Code == Utils::PathValidationErrorCode::AbsolutePath,
                "An absolute audio path was accepted.");

            const std::filesystem::path outsideFixture = temporaryDirectory.GetPath() / "Outside.wav";
            const std::filesystem::path linkPath = projectRoot / "Assets/Audio/Escape.wav";
            WriteFixture(outsideFixture);
            std::error_code linkError;
            std::filesystem::create_symlink(outsideFixture, linkPath, linkError);
            if (linkError)
            {
                Require(IsExpectedSymlinkCreationFailure(linkError),
                    "The audio symlink fixture failed for an unexpected reason.");
            }
            else
            {
                result = TryResolveProjectAudioPath(projectRoot, "Assets/Audio/Escape.wav", resolvedPath);
                Require(!result && result.Code == Utils::PathValidationErrorCode::OutsideRoot,
                    "An audio symlink escaping the project root was accepted.");
            }
        }

        void VerifyInjectedBackendLifecycle()
        {
            auto state = std::make_shared<FakeAudioBackendState>();
            {
                AudioEngine audio(CreateScope<FakeAudioBackend>(state));
                Require(!audio.Initialize(), "The injected backend's first failure was ignored.");
                Require(!audio.IsInitialized(), "A failed injected backend was reported initialized.");
                Require(audio.GetLastError() == "injected initialization failure",
                    "The injected backend error was not forwarded.");

                audio.Shutdown();
                audio.Shutdown();
                Require(state->ShutdownCalls == 2,
                    "Repeated shutdown did not reach the injected idempotent backend.");

                Require(audio.Initialize(), "The injected backend could not retry initialization.");
                Require(audio.IsInitialized() && audio.IsDeviceAvailable(),
                    "A successful injected backend was not observable.");

                const auto spatial = ReadScriptAudioParams(Json{{"loop", true}, {"bus", 2}, {"position", {1, 2, 3}},
                    {"volume", 0.25f}, {"pitch", 1.5f}, {"paused", true}, {"minDistance", 2}, {"maxDistance", 20}});
                Require(spatial.bSpatialized && spatial.bLooping && spatial.bStream && spatial.bStartPaused &&
                    spatial.Bus == AudioBus::UI && spatial.Position == glm::vec3(1, 2, 3) && spatial.Pitch == 1.5f,
                    "Script spatial audio options lost their typed values.");
                Require(ReadScriptAudioParams(Json{{"loop", true}}).Bus == AudioBus::Music,
                    "Legacy looped audio no longer defaults to the music bus.");
                for (const auto& invalid : {Json{{"bus", -1}}, Json{{"attenuation", 4}}, Json{{"pitch", 0}},
                    Json{{"volume", 2}}, Json{{"pan", -2}}, Json{{"minDistance", 5}, {"maxDistance", 1}}, Json{{"position", {1, 2}}}})
                {
                    bool rejected = false;
                    try { (void)ReadScriptAudioParams(invalid); } catch (const std::exception&) { rejected = true; }
                    Require(rejected, "Invalid script audio options reached the backend.");
                }
                Require(ExecuteScriptAudioControl(audio, "pause", Json::object(), {41}) == true && state->bPaused,
                    "Script pause did not reach the audio backend.");
                Require(ExecuteScriptAudioControl(audio, "resume", Json::object(), {41}) == true && !state->bPaused,
                    "Script resume did not reach the audio backend.");
                Require(ExecuteScriptAudioControl(audio, "length", Json::object(), {41}) == 2.5 &&
                    ExecuteScriptAudioControl(audio, "playbackPosition", Json::object(), {41}) == 1.25,
                    "Script audio duration/cursor contract changed.");
                Require(ExecuteScriptAudioControl(audio, "busVolume", Json{{"bus", 2}, {"value", 0.25f}}) == 0.25f &&
                    state->BusVolumes[2] == 0.25f, "Script bus gain did not reach the selected bus.");
                Require(ExecuteScriptAudioControl(audio, "pause", Json::object(), {0}) == false,
                    "An absent audio handle was reported as paused.");
                bool rejectedSeek = false;
                try { (void)ExecuteScriptAudioControl(audio, "seek", Json{{"seconds", -1}}, {41}); }
                catch (const std::invalid_argument&) { rejectedSeek = true; }
                Require(rejectedSeek, "Negative script seek was accepted.");

                AudioPlayParams params;
                params.Bus = AudioBus::UI;
                params.bSpatialized = true;
                const AudioHandle handle = audio.Play("Injected.wav", params);
                Require(handle.Value == 41 && state->LastPath == std::filesystem::path("Injected.wav"),
                    "File playback was not forwarded to the injected backend.");

                const glm::vec3 velocity{ 1.0f, 2.0f, 3.0f };
                Require(audio.SetVelocity(handle, velocity) && state->LastVelocity == velocity,
                    "Spatial velocity was not forwarded to the injected backend.");
                Require(audio.Pause(handle, true) && audio.IsPaused(handle),
                    "Pause state was not forwarded to the injected backend.");
                Require(audio.Pause(handle, false) && audio.IsPlaying(handle),
                    "Resume state was not forwarded to the injected backend.");
                Require(audio.Stop(handle) && state->LastStoppedHandle == handle,
                    "Stop was not forwarded to the injected backend.");

                audio.SetGameplayPaused(true);
                Require(audio.IsGameplayPaused(), "Gameplay bus pause was not forwarded.");
                audio.Update();
                Require(state->UpdateCalls == 1, "Audio update was not forwarded.");

                audio.Shutdown();
                Require(!audio.IsInitialized(), "Shutdown left the injected backend initialized.");
                Require(audio.Initialize(), "The injected backend could not initialize after shutdown.");
            }

            Require(state->InitializeCalls == 3,
                "Injected backend initialization attempts did not match fail/retry/reinitialize.");
            Require(state->ShutdownCalls == 4 && !state->bInitialized,
                "AudioEngine destruction did not leave the injected backend shut down.");
        }
    }

    void AudioLifecycleContract()
    {
        VerifyStrictProjectAudioPaths();
        VerifyInjectedBackendLifecycle();
    }
}
