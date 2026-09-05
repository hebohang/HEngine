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
    class AudioBackend
    {
    public:
        virtual ~AudioBackend() = default;

        virtual bool Initialize() = 0;
        virtual void Shutdown() = 0;
        virtual void Update() = 0;

        [[nodiscard]] virtual bool IsInitialized() const = 0;
        [[nodiscard]] virtual bool IsDeviceAvailable() const = 0;
        [[nodiscard]] virtual std::string GetLastError() const = 0;

        virtual AudioHandle Play(
            const std::filesystem::path& path,
            const AudioPlayParams& params) = 0;
        virtual AudioHandle PlayMemory(
            std::shared_ptr<std::vector<uint8_t>> data,
            const std::string& debugName,
            const AudioPlayParams& params) = 0;

        virtual bool Stop(AudioHandle handle) = 0;
        virtual bool Pause(AudioHandle handle, bool bPaused) = 0;
        [[nodiscard]] virtual bool IsPlaying(AudioHandle handle) const = 0;
        [[nodiscard]] virtual bool IsPaused(AudioHandle handle) const = 0;
        [[nodiscard]] virtual AudioPlaybackState GetState(AudioHandle handle) const = 0;

        virtual bool SetVolume(AudioHandle handle, float volume) = 0;
        virtual bool SetPitch(AudioHandle handle, float pitch) = 0;
        virtual bool SetPan(AudioHandle handle, float pan) = 0;
        virtual bool SetPosition(AudioHandle handle, const glm::vec3& position) = 0;
        virtual bool SetDirection(AudioHandle handle, const glm::vec3& direction) = 0;
        virtual bool SetVelocity(AudioHandle handle, const glm::vec3& velocity) = 0;
        virtual bool SetCone(
            AudioHandle handle,
            float innerAngleRadians,
            float outerAngleRadians,
            float outerGain) = 0;
        virtual bool Seek(AudioHandle handle, double seconds) = 0;
        [[nodiscard]] virtual double GetPlaybackPosition(AudioHandle handle) const = 0;
        [[nodiscard]] virtual double GetLength(AudioHandle handle) const = 0;

        virtual void SetMasterVolume(float volume) = 0;
        [[nodiscard]] virtual float GetMasterVolume() const = 0;
        virtual void SetBusVolume(AudioBus bus, float volume) = 0;
        [[nodiscard]] virtual float GetBusVolume(AudioBus bus) const = 0;
        virtual void StopBus(AudioBus bus) = 0;
        virtual void SetGameplayPaused(bool bPaused) = 0;
        [[nodiscard]] virtual bool IsGameplayPaused() const = 0;

        virtual bool SetListener(uint32_t index, const AudioListener& listener) = 0;
    };

    Scope<AudioBackend> CreatePlatformAudioBackend();
}
