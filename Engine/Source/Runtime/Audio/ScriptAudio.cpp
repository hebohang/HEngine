#include "ScriptAudio.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        float Number(const Json& args, const char* key, float fallback, float minimum, float maximum)
        {
            const auto value = args.value(key, fallback);
            if (!std::isfinite(value) || value < minimum || value > maximum)
                throw std::invalid_argument(std::string("Invalid audio ") + key + ".");
            return value;
        }
        glm::vec3 Vector(const Json& args, const char* key, glm::vec3 fallback = {})
        {
            if (!args.contains(key)) return fallback;
            const auto& value = args.at(key);
            if (!value.is_array() || value.size() != 3) throw std::invalid_argument("Audio vectors require three numbers.");
            glm::vec3 result(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
            if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
                throw std::invalid_argument("Audio vectors must be finite.");
            return result;
        }
        AudioBus Bus(const Json& args, AudioBus fallback = AudioBus::Gameplay)
        {
            const auto value = args.value("bus", static_cast<int>(fallback));
            if (value < 0 || value >= static_cast<int>(AudioBus::Count)) throw std::invalid_argument("Unknown audio bus.");
            return static_cast<AudioBus>(value);
        }
        constexpr float Max = std::numeric_limits<float>::max();
        constexpr float Tau = 6.28318530718f;
    }

    AudioPlayParams ReadScriptAudioParams(const Json& args)
    {
        AudioPlayParams params;
        params.bLooping = args.value("loop", false);
        params.bStream = args.value("stream", params.bLooping);
        params.bStartPaused = args.value("paused", false);
        // Preserve the original SceneServices.PlayAudio routing when bus is omitted.
        params.Bus = Bus(args, params.bLooping ? AudioBus::Music : AudioBus::Gameplay);
        params.Volume = Number(args, "volume", 1, 0, 1);
        params.Pitch = Number(args, "pitch", 1, 0.001f, Max);
        params.Pan = Number(args, "pan", 0, -1, 1);
        params.bSpatialized = args.contains("position");
        params.Position = Vector(args, "position");
        params.Direction = Vector(args, "direction", {0, 0, -1});
        params.Velocity = Vector(args, "velocity");
        params.MinDistance = Number(args, "minDistance", 1, 0, Max);
        params.MaxDistance = Number(args, "maxDistance", 100, params.MinDistance, Max);
        params.Rolloff = Number(args, "rolloff", 1, 0, Max);
        params.DopplerFactor = Number(args, "dopplerFactor", 1, 0, Max);
        const auto attenuation = args.value("attenuation", 1);
        if (attenuation < 0 || attenuation >= static_cast<int>(AudioAttenuationModel::Count))
            throw std::invalid_argument("Unknown audio attenuation model.");
        params.Attenuation = static_cast<AudioAttenuationModel>(attenuation);
        return params;
    }

    Json ExecuteScriptAudioControl(AudioEngine& audio, const std::string& operation, const Json& args, AudioHandle handle)
    {
        if (operation == "stop") return audio.Stop(handle);
        if (operation == "pause") return audio.Pause(handle, true);
        if (operation == "resume") return audio.Pause(handle, false);
        if (operation == "isPlaying") return audio.IsPlaying(handle);
        if (operation == "isPaused") return audio.IsPaused(handle);
        if (operation == "state") return static_cast<int>(audio.GetState(handle));
        if (operation == "volume") return audio.SetVolume(handle, Number(args, "value", 1, 0, 1));
        if (operation == "pitch") return audio.SetPitch(handle, Number(args, "value", 1, 0.001f, Max));
        if (operation == "pan") return audio.SetPan(handle, Number(args, "value", 0, -1, 1));
        if (operation == "position") return audio.SetPosition(handle, Vector(args, "value"));
        if (operation == "velocity") return audio.SetVelocity(handle, Vector(args, "value"));
        if (operation == "direction") return audio.SetDirection(handle, Vector(args, "value", {0, 0, -1}));
        if (operation == "cone")
        {
            const auto inner = Number(args, "inner", Tau, 0, Tau);
            return audio.SetCone(handle, inner, Number(args, "outer", Tau, inner, Tau), Number(args, "gain", 1, 0, 1));
        }
        if (operation == "seek")
        {
            const auto seconds = args.at("seconds").get<double>();
            if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("Audio seek requires finite nonnegative seconds.");
            return audio.Seek(handle, seconds);
        }
        if (operation == "playbackPosition") return audio.GetPlaybackPosition(handle);
        if (operation == "length") return audio.GetLength(handle);
        if (operation == "masterVolume")
        {
            if (args.contains("value")) audio.SetMasterVolume(Number(args, "value", 1, 0, 1));
            return audio.GetMasterVolume();
        }
        if (operation == "busVolume")
        {
            const auto bus = Bus(args);
            if (args.contains("value")) audio.SetBusVolume(bus, Number(args, "value", 1, 0, 1));
            return audio.GetBusVolume(bus);
        }
        if (operation == "stopBus") { audio.StopBus(Bus(args)); return true; }
        if (operation == "deviceAvailable") return audio.IsDeviceAvailable();
        if (operation == "lastError") return audio.GetLastError();
        if (operation == "listener")
        {
            AudioListener listener;
            listener.Position = Vector(args, "position");
            listener.Forward = Vector(args, "forward", {0, 0, -1});
            listener.Up = Vector(args, "up", {0, 1, 0});
            listener.Velocity = Vector(args, "velocity");
            if (glm::dot(listener.Forward, listener.Forward) < 1e-8f || glm::dot(listener.Up, listener.Up) < 1e-8f ||
                glm::length(glm::cross(listener.Forward, listener.Up)) < 1e-4f)
                throw std::invalid_argument("Listener forward and up must be nonzero and independent.");
            return audio.SetListener(0, listener);
        }
        throw std::invalid_argument("Unknown audio operation: " + operation);
    }
}
