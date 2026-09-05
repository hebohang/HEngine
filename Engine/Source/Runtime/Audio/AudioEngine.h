#pragma once

#include "Runtime/Audio/AudioTypes.h"
#include "Runtime/Core/Base/Base.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace HEngine
{
    class AudioBackend;

    class AudioEngine
    {
    public:
        static AudioEngine& GetInstance();

        explicit AudioEngine(Scope<AudioBackend> backend);
        ~AudioEngine();

        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        bool Initialize();
        void Shutdown();
        void Update();

        [[nodiscard]] bool IsInitialized() const;
        [[nodiscard]] bool IsDeviceAvailable() const;
        [[nodiscard]] std::string GetLastError() const;

        AudioHandle Play(
            const std::filesystem::path& path,
            const AudioPlayParams& params = {});
        AudioHandle PlayMemory(
            std::shared_ptr<std::vector<uint8_t>> data,
            const std::string& debugName,
            const AudioPlayParams& params = {});

        bool Stop(AudioHandle handle);
        bool Pause(AudioHandle handle, bool bPaused = true);
        [[nodiscard]] bool IsPlaying(AudioHandle handle) const;
        [[nodiscard]] bool IsPaused(AudioHandle handle) const;
        [[nodiscard]] AudioPlaybackState GetState(AudioHandle handle) const;

        bool SetVolume(AudioHandle handle, float volume);
        bool SetPitch(AudioHandle handle, float pitch);
        bool SetPan(AudioHandle handle, float pan);
        bool SetPosition(AudioHandle handle, const glm::vec3& position);
        bool SetDirection(AudioHandle handle, const glm::vec3& direction);
        bool SetVelocity(AudioHandle handle, const glm::vec3& velocity);
        bool SetCone(
            AudioHandle handle,
            float innerAngleRadians,
            float outerAngleRadians,
            float outerGain);
        bool Seek(AudioHandle handle, double seconds);
        [[nodiscard]] double GetPlaybackPosition(AudioHandle handle) const;
        [[nodiscard]] double GetLength(AudioHandle handle) const;

        void SetMasterVolume(float volume);
        [[nodiscard]] float GetMasterVolume() const;
        void SetBusVolume(AudioBus bus, float volume);
        [[nodiscard]] float GetBusVolume(AudioBus bus) const;
        void StopBus(AudioBus bus);
        void SetGameplayPaused(bool bPaused);
        [[nodiscard]] bool IsGameplayPaused() const;

        bool SetListener(uint32_t index, const AudioListener& listener);

    private:
        AudioEngine();

        struct Impl;
        Scope<Impl> mImpl;
    };
}
