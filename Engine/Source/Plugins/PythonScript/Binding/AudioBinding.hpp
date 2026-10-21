#pragma once

#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Audio/ProjectAudioPath.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <pybind11/embed.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace py = pybind11;

namespace HEngine
{
    static AudioHandle PlayProjectAudio(
        const std::string& path,
        const AudioPlayParams& params)
    {
        ProjectAudioPath audioPath;
        const Utils::PathValidationResult result = TryResolveProjectAudioPath(
            ConfigManager::GetInstance().GetRootFolder(),
            path,
            audioPath);
        if (!result)
        {
            HE_CORE_WARN("Rejected project audio path '{0}': {1}", path, result.Message);
            return {};
        }
        return AudioEngine::GetInstance().Play(audioPath.FullPath, params);
    }

    static void AudioBinding(py::module_& HEngineModule)
    {
        py::module_ audio = HEngineModule.def_submodule("Audio");

        py::enum_<AudioBus>(audio, "Bus")
            .value("Music", AudioBus::Music)
            .value("Gameplay", AudioBus::Gameplay)
            .value("UI", AudioBus::UI)
            .export_values();

        py::enum_<AudioAttenuationModel>(audio, "Attenuation")
            .value("Disabled", AudioAttenuationModel::None)
            .value("Inverse", AudioAttenuationModel::Inverse)
            .value("Linear", AudioAttenuationModel::Linear)
            .value("Exponential", AudioAttenuationModel::Exponential)
            .export_values();

        audio.def(
            "Play",
            [](const std::string& path, AudioBus bus, float volume, float pitch,
                bool bLooping, bool bStream, float pan)
            {
                AudioPlayParams params;
                params.Bus = bus;
                params.Volume = volume;
                params.Pitch = pitch;
                params.Pan = pan;
                params.bLooping = bLooping;
                params.bStream = bStream;
                return PlayProjectAudio(path, params).Value;
            },
            py::arg("path"),
            py::arg("bus") = AudioBus::Gameplay,
            py::arg("volume") = 1.0f,
            py::arg("pitch") = 1.0f,
            py::arg("loop") = false,
            py::arg("stream") = false,
            py::arg("pan") = 0.0f);

        audio.def(
            "Play3D",
            [](const std::string& path, const glm::vec3& position, AudioBus bus,
                float volume, float pitch, float minDistance, float maxDistance,
                float rolloff, bool bLooping, bool bStream,
                AudioAttenuationModel attenuation, float dopplerFactor)
            {
                AudioPlayParams params;
                params.Bus = bus;
                params.Position = position;
                params.Volume = volume;
                params.Pitch = pitch;
                params.MinDistance = minDistance;
                params.MaxDistance = maxDistance;
                params.Rolloff = rolloff;
                params.Attenuation = attenuation;
                params.DopplerFactor = dopplerFactor;
                params.bLooping = bLooping;
                params.bSpatialized = true;
                params.bStream = bStream;
                return PlayProjectAudio(path, params).Value;
            },
            py::arg("path"),
            py::arg("position"),
            py::arg("bus") = AudioBus::Gameplay,
            py::arg("volume") = 1.0f,
            py::arg("pitch") = 1.0f,
            py::arg("min_distance") = 1.0f,
            py::arg("max_distance") = 50.0f,
            py::arg("rolloff") = 1.0f,
            py::arg("loop") = false,
            py::arg("stream") = false,
            py::arg("attenuation") = AudioAttenuationModel::Inverse,
            py::arg("doppler_factor") = 1.0f);

        audio.def("Stop", [](uint64_t handle) { return AudioEngine::GetInstance().Stop({ handle }); });
        audio.def("Pause", [](uint64_t handle) { return AudioEngine::GetInstance().Pause({ handle }, true); });
        audio.def("Resume", [](uint64_t handle) { return AudioEngine::GetInstance().Pause({ handle }, false); });
        audio.def("IsPlaying", [](uint64_t handle) { return AudioEngine::GetInstance().IsPlaying({ handle }); });
        audio.def("IsPaused", [](uint64_t handle) { return AudioEngine::GetInstance().IsPaused({ handle }); });
        audio.def("SetPosition", [](uint64_t handle, const glm::vec3& position)
        {
            return AudioEngine::GetInstance().SetPosition({ handle }, position);
        });
        audio.def("SetVelocity", [](uint64_t handle, const glm::vec3& velocity)
        {
            return AudioEngine::GetInstance().SetVelocity({ handle }, velocity);
        });
        audio.def("SetDirection", [](uint64_t handle, const glm::vec3& direction)
        {
            return AudioEngine::GetInstance().SetDirection({ handle }, direction);
        });
        audio.def(
            "SetCone",
            [](uint64_t handle, float innerAngleRadians, float outerAngleRadians, float outerGain)
            {
                return AudioEngine::GetInstance().SetCone(
                    { handle }, innerAngleRadians, outerAngleRadians, outerGain);
            },
            py::arg("handle"),
            py::arg("inner_angle_radians"),
            py::arg("outer_angle_radians"),
            py::arg("outer_gain"));
        audio.def("SetVolume", [](uint64_t handle, float volume)
        {
            return AudioEngine::GetInstance().SetVolume({ handle }, volume);
        });
        audio.def("SetPitch", [](uint64_t handle, float pitch)
        {
            return AudioEngine::GetInstance().SetPitch({ handle }, pitch);
        });
        audio.def("SetPan", [](uint64_t handle, float pan)
        {
            return AudioEngine::GetInstance().SetPan({ handle }, pan);
        });
        audio.def("Seek", [](uint64_t handle, double seconds)
        {
            return AudioEngine::GetInstance().Seek({ handle }, seconds);
        });
        audio.def("GetPlaybackPosition", [](uint64_t handle)
        {
            return AudioEngine::GetInstance().GetPlaybackPosition({ handle });
        });
        audio.def("GetLength", [](uint64_t handle)
        {
            return AudioEngine::GetInstance().GetLength({ handle });
        });

        audio.def("SetMasterVolume", [](float volume) { AudioEngine::GetInstance().SetMasterVolume(volume); });
        audio.def("GetMasterVolume", []() { return AudioEngine::GetInstance().GetMasterVolume(); });
        audio.def("SetBusVolume", [](AudioBus bus, float volume) { AudioEngine::GetInstance().SetBusVolume(bus, volume); });
        audio.def("GetBusVolume", [](AudioBus bus) { return AudioEngine::GetInstance().GetBusVolume(bus); });
        audio.def("StopBus", [](AudioBus bus) { AudioEngine::GetInstance().StopBus(bus); });
        audio.def("IsDeviceAvailable", []() { return AudioEngine::GetInstance().IsDeviceAvailable(); });
        audio.def("GetLastError", []() { return AudioEngine::GetInstance().GetLastError(); });

        audio.def(
            "SetListener",
            [](const glm::vec3& position, const glm::vec3& forward,
                const glm::vec3& up, const glm::vec3& velocity)
            {
                AudioListener listener;
                listener.Position = position;
                listener.Forward = forward;
                listener.Up = up;
                listener.Velocity = velocity;
                return AudioEngine::GetInstance().SetListener(0, listener);
            },
            py::arg("position"),
            py::arg("forward") = glm::vec3(0.0f, 0.0f, -1.0f),
            py::arg("up") = glm::vec3(0.0f, 1.0f, 0.0f),
            py::arg("velocity") = glm::vec3(0.0f),
            "Sets listener 0 immediately. During scene updates, an active AudioListenerComponent "
            "(or the primary camera fallback) remains authoritative and overwrites this value.");
    }
}
