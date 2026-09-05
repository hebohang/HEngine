#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace HEngine
{
    struct AudioHandle
    {
        uint64_t Value = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept { return Value != 0; }
        constexpr explicit operator bool() const noexcept { return IsValid(); }

        friend constexpr bool operator==(AudioHandle, AudioHandle) noexcept = default;
    };

    enum class AudioBus : uint8_t
    {
        Music = 0,
        Gameplay = 1,
        UI = 2,
        Count = 3
    };

    enum class AudioPlaybackState : uint8_t
    {
        Invalid = 0,
        Playing = 1,
        Paused = 2,
        Stopped = 3,
        Count = 4
    };

    enum class AudioAttenuationModel : uint8_t
    {
        None = 0,
        Inverse = 1,
        Linear = 2,
        Exponential = 3,
        Count = 4
    };

    struct AudioListener
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Forward{ 0.0f, 0.0f, -1.0f };
        glm::vec3 Up{ 0.0f, 1.0f, 0.0f };
        glm::vec3 Velocity{ 0.0f };
        bool bEnabled = true;
    };

    struct AudioPlayParams
    {
        AudioBus Bus = AudioBus::Gameplay;
        glm::vec3 Position{ 0.0f };
        glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
        glm::vec3 Velocity{ 0.0f };
        AudioAttenuationModel Attenuation = AudioAttenuationModel::Inverse;
        float Volume = 1.0f;
        float Pitch = 1.0f;
        float Pan = 0.0f;
        float MinDistance = 1.0f;
        float MaxDistance = 100.0f;
        float Rolloff = 1.0f;
        float DopplerFactor = 1.0f;
        float ConeInnerAngleRadians = 6.28318530718f;
        float ConeOuterAngleRadians = 6.28318530718f;
        float ConeOuterGain = 1.0f;
        double StartTimeSeconds = 0.0;
        bool bLooping = false;
        bool bSpatialized = false;
        bool bStream = false;
        bool bStartPaused = false;
    };
}
